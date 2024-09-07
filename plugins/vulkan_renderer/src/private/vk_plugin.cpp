#include "vk_plugin.hpp"

#include "math.hpp"
#include "pipeline_builder.hpp"

#include "SDL_main.h"
#include "vk_types.hpp"
#include <cstddef>
#include <fstream>

#define VMA_IMPLEMENTATION
#include <vma/vk_mem_alloc.h>
#include <SDL.h>
#include <SDL_vulkan.h>

#include <VkBootstrap.h>

#include <glm/glm.hpp>
#include <glm/ext.hpp>

#include <filesystem>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_vulkan.h"

#include "vk_verify.hpp"
#include "vk_loader.hpp"

#include "defines.hpp"
#include "ecs/app.hpp"
#include "gameplay_framework.hpp"
#include "vulkan/vulkan_core.h"
// @TMP: All of this is tmp until I can come up with better abstractions.

static_assert(std::same_as<decltype(null), decltype(VK_NULL_HANDLE)>);

// @NOTE: This is suboptimal.
struct DeletionQueue {
	auto enqueue(Fn<void()>&& dtor) -> void {
		dtors.push_back(std::move(dtor));
	}

	auto flush() -> void {
		// Invoke dtors FIFO.
		for (auto it = dtors.rbegin(); it != dtors.rend(); ++it) {
			(*it)();
		}
		dtors.clear();
	}

	Queue<Fn<void()>> dtors;
};

struct AllocatedImage {
	VkImage image = null;
	VkImageView image_view = null;
	VmaAllocation allocation = null;
	VkExtent3D extent = {};
	VkFormat format;
};

struct FrameData {
	VkCommandPool command_pool = null;
	VkCommandBuffer command_buffer = null;

	VkSemaphore swapchain_semaphore = null;
	VkSemaphore render_semaphore = null;
	VkFence render_fence = null;

	DeletionQueue deletion_queue;
	DescriptorAllocatorGrowable descriptors;
};

struct ComputePushConstants {
	glm::vec4 data1, data2, data3, data4;
};

struct ComputeEffect {
	const char* name;

	VkPipeline pipeline;
	VkPipelineLayout layout;

	ComputePushConstants data;
};

struct DescriptorLayoutBuilder {
	constexpr auto add_binding(const u32 binding, const VkDescriptorType type) -> DescriptorLayoutBuilder& {
		bindings.push_back(VkDescriptorSetLayoutBinding{
			.binding = binding,
			.descriptorType = type,
			.descriptorCount = 1,
		});

		return *this;
	}

	constexpr auto clear() -> void {
		bindings.clear();
	}

	[[nodiscard]] constexpr auto build(VkDevice device, const VkShaderStageFlags shader_stages, void* p_next = null, const VkDescriptorSetLayoutCreateFlags create_flags = 0) -> VkDescriptorSetLayout {
		for (auto& binding : bindings) {
			binding.stageFlags |= shader_stages;
		}

		const VkDescriptorSetLayoutCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.pNext = p_next,
			.flags = create_flags,
			.bindingCount = static_cast<u32>(bindings.size()),
			.pBindings = bindings.data(),
		};

		VkDescriptorSetLayout out;
		VK_VERIFY(vkCreateDescriptorSetLayout(device, &create_info, null, &out));

		return out;
	}

	Array<VkDescriptorSetLayoutBinding> bindings;
};

struct DescriptorAllocator {
	struct PoolSizeRatio {
		VkDescriptorType type;
		f32 ratio;
	};

	VkDescriptorPool pool;

	auto init_pool(VkDevice device, const u32 max_sets, const Span<const PoolSizeRatio> pool_ratios) -> void {
		Array<VkDescriptorPoolSize> pool_sizes;
		pool_sizes.reserve(pool_ratios.size());
		for (const PoolSizeRatio& pool_ratio : pool_ratios) {
			pool_sizes.push_back(VkDescriptorPoolSize{
				.type = pool_ratio.type,
				.descriptorCount = static_cast<u32>(pool_ratio.ratio * max_sets),
			});
		}

		const VkDescriptorPoolCreateInfo create_info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = max_sets,
			.poolSizeCount = static_cast<u32>(pool_sizes.size()),
			.pPoolSizes = pool_sizes.data(),
		};

		VK_VERIFY(vkCreateDescriptorPool(device, &create_info, null, &pool));
	}

	auto clear_descriptors(VkDevice device) -> void {
		VK_VERIFY(vkResetDescriptorPool(device, pool, 0));
	}

	auto destroy_pool(VkDevice device) -> void {
		vkDestroyDescriptorPool(device, pool, null);
	}

	[[nodiscard]] auto allocate(VkDevice device, VkDescriptorSetLayout layout) -> VkDescriptorSet {
		ASSERT(layout);

		const VkDescriptorSetAllocateInfo alloc_info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = pool,
			.descriptorSetCount = 1,
			.pSetLayouts = &layout,
		};

		VkDescriptorSet out;
		VK_VERIFY(vkAllocateDescriptorSets(device, &alloc_info, &out));

		return out;
	}
};

// Number of frames in-flight.
static constexpr usize FRAMES_IN_FLIGHT_COUNT = 2;

namespace vkinit {
static auto make_attachment_info(VkImageView image_view, const Optional<VkClearValue>& clear_value = NULL_OPTIONAL, const VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) -> VkRenderingAttachmentInfo {
	VkRenderingAttachmentInfo out{
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = image_view,
		.imageLayout = layout,
		.loadOp = clear_value ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	};

	if (clear_value) {
		out.clearValue = *clear_value;
	}

	return out;
}

static auto make_depth_attachment_info(VkImageView image_view, const VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) -> VkRenderingAttachmentInfo {
	return VkRenderingAttachmentInfo{
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = image_view,
		.imageLayout = layout,
		// Apply clear color (zero-out depth of previous value).
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue{
			.depthStencil{
				// Zero out depth.
				.depth = 0.f,
			},
		},
	};
}

static auto make_image_create_info(const VkFormat format, const VkImageUsageFlags usage_flags, const VkExtent3D extent) -> VkImageCreateInfo {
	return {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
		.extent = extent,
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,// MSAA count.
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage_flags,
	};
}

static auto make_image_view_create_info(const VkFormat format, const VkImage image, const VkImageAspectFlags aspect_flags) -> VkImageViewCreateInfo {
	ASSERT(image);
	return {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = format,
		.subresourceRange{
			.aspectMask = aspect_flags,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
	};
}
}

namespace vkutils {
static auto load_shader_module(const char* file_path, VkDevice device) -> VkShaderModule {
	ASSERTF(StringView{file_path}.contains(".spv"), "File path for shader module {} must have the .spv extension!", file_path);

	std::ifstream file{file_path, std::ios::ate | std::ios::binary};
	ASSERTF(file.is_open(), "Failed to open file {}!", file_path);

	const usize file_size = static_cast<usize>(file.tellg());

	// Spir-V expects the buffer data as u32s.
	Array<u32> buffer;
	buffer.resize(math::divide_and_round_up(file_size, sizeof(u32)));
	//buffer.reserve(math::divide_and_round_up(file_size, sizeof(u32)));

	// Put file cursor at the beginning of the file.
	file.seekg(0);

	// Read the entire file into the buffer.
	file.read(reinterpret_cast<char*>(buffer.data()), file_size);

	file.close();

	const VkShaderModuleCreateInfo shader_module_create_info{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = buffer.size() * sizeof(u32),
		.pCode = buffer.data(),
	};

	VkShaderModule out;
	VK_VERIFY(vkCreateShaderModule(device, &shader_module_create_info, null, &out));

	return out;
}

static auto copy_image_to_image(VkCommandBuffer cmd, VkImage src, VkImage dst, const VkExtent2D& src_extent, const VkExtent2D& dst_extent) -> void {
	const VkImageBlit2 blit_region{
		.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
		.srcSubresource{
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.mipLevel = 0,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
		.srcOffsets{
			VkOffset3D{
				0, 0, 0,
			},
			VkOffset3D{
				.x = static_cast<i32>(src_extent.width),
				.y = static_cast<i32>(src_extent.height),
				.z = 1,
			},
		},
		.dstSubresource{
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.mipLevel = 0,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
		.dstOffsets{
			VkOffset3D{
				0, 0, 0,
			},
			VkOffset3D{
				.x = static_cast<i32>(dst_extent.width),
				.y = static_cast<i32>(dst_extent.height),
				.z = 1,
			},
		},
	};

	const VkBlitImageInfo2 blit_info{
		.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
		.srcImage = src,
		.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.dstImage = dst,
		.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.regionCount = 1,
		.pRegions = &blit_region,
		.filter = VK_FILTER_LINEAR,
	};

	vkCmdBlitImage2(cmd, &blit_info);
}

// @NOTE: VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT is slow. It will do a large amount of stalling.
static auto transition_image_layout(VkCommandBuffer cmd, VkImage image, const VkImageLayout current_layout, const VkImageLayout new_layout) -> void {
	// Special case for transitioning to depth texture.
	const VkImageAspectFlags aspect_mask = new_layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

	const VkImageMemoryBarrier2 image_barrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,

		.oldLayout = current_layout,
		.newLayout = new_layout,

		.image = image,

		.subresourceRange{// For applying the barrier access for specific mip levels. Just applying to all mips for now.
			.aspectMask = aspect_mask,
			.baseMipLevel = 0,
			.levelCount = VK_REMAINING_MIP_LEVELS,
			.baseArrayLayer = 0,
			.layerCount = VK_REMAINING_ARRAY_LAYERS,
		},
	};

	const VkDependencyInfo dependency_info{
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &image_barrier,
	};

	vkCmdPipelineBarrier2(cmd, &dependency_info);
}
}

namespace res {
struct VkEngine {
	[[nodiscard]] auto get_current_frame_data(this auto&& self) -> auto& {
		return self.frames[self.current_frame_count % FRAMES_IN_FLIGHT_COUNT];
	}

	auto init(this VkEngine& self, const WindowConfig& window_config) -> void {
		static constexpr auto WINDOW_FLAGS = static_cast<SDL_WindowFlags>(SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
		self.window = SDL_CreateWindow(window_config.title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, window_config.extent.width, window_config.extent.height, WINDOW_FLAGS);

		// Create vk instance.
		vkb::InstanceBuilder instance_builder;

		auto instance = instance_builder
			.set_app_name("GanEngine")
			.request_validation_layers(DEBUG_BUILD)// Enable validation layers in debug-builds.
			.use_default_debug_messenger()
			.require_api_version(1, 3, 0)
			.build()
			.value();

		self.instance = instance.instance;
		self.debug_messenger = instance.debug_messenger;

		ASSERT(self.instance);
		ASSERT(self.debug_messenger);

		// Create surface to render to.
		ASSERT(!self.surface);
		SDL_Vulkan_CreateSurface(self.window, self.instance, &self.surface);
		ASSERT(self.surface);

		const VkPhysicalDeviceVulkan13Features features_1_3{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
			.synchronization2 = VK_TRUE,
			.dynamicRendering = VK_TRUE,
		};

		const VkPhysicalDeviceVulkan12Features features_1_2{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
			.descriptorIndexing = VK_TRUE,
			.bufferDeviceAddress = VK_TRUE,
		};

		// Let vkb select the most appropriate GPU.
		vkb::PhysicalDeviceSelector selector{instance};
		vkb::PhysicalDevice physical_device = selector
			.set_minimum_version(1, 3)
			.set_required_features_13(features_1_3)
			.set_required_features_12(features_1_2)
			.set_surface(self.surface)
			.select()
			.value();

		vkb::DeviceBuilder device_builder{physical_device};
		vkb::Device device = device_builder.build().value();

		self.physical_device = physical_device.physical_device;
		self.device = device.device;

		// Init allocator.
		const VmaAllocatorCreateInfo alloc_create_info{
			.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
			.physicalDevice = self.physical_device,
			.device = self.device,
			.instance = self.instance,
		};

		VK_VERIFY(vmaCreateAllocator(&alloc_create_info, &self.allocator));

		self.deletion_queue.enqueue([&self] {
			vmaDestroyAllocator(self.allocator);
		});

		// Init swapchain.
		self.init_swapchain(window_config.extent.width, window_config.extent.height);

		// Get the graphics queue.
		self.graphics_queue = device.get_queue(vkb::QueueType::graphics).value();
		self.graphics_queue_family = device.get_queue_index(vkb::QueueType::graphics).value();

		// Init commands.
		self.init_commands();

		self.init_descriptors();
		self.init_pipelines();

		self.init_imgui();

		self.init_default_data();
	}

	auto init_swapchain(const u32 width, const u32 height) -> void {
		create_swapchain(width, height);

		// Create draw image.
		const VkExtent3D draw_extent_3d{
			width,
			height,
			1,
		};

		draw_image.format = VK_FORMAT_R16G16B16A16_SFLOAT;
		draw_image.extent = draw_extent_3d;

		static constexpr auto USAGE_FLAGS = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		const VkImageCreateInfo image_create_info = vkinit::make_image_create_info(draw_image.format, USAGE_FLAGS, draw_image.extent);

		const VmaAllocationCreateInfo alloc_info{
			.usage = VMA_MEMORY_USAGE_GPU_ONLY,
			.requiredFlags = static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
		};

		// Allocate and create the image.
		ASSERT(allocator);
		vmaCreateImage(allocator, &image_create_info, &alloc_info, &draw_image.image, &draw_image.allocation, null);

		const VkImageViewCreateInfo image_view_create_info = vkinit::make_image_view_create_info(draw_image.format, draw_image.image, VK_IMAGE_ASPECT_COLOR_BIT);

		VK_VERIFY(vkCreateImageView(device, &image_view_create_info, null, &draw_image.image_view));

		// Create depth image for the draw_image.
		depth_image.format = VK_FORMAT_D32_SFLOAT;
		depth_image.extent = draw_image.extent;

		static constexpr auto DEPTH_USAGE_FLAGS = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

		const VkImageCreateInfo depth_image_create_info = vkinit::make_image_create_info(depth_image.format, DEPTH_USAGE_FLAGS, draw_image.extent);

		VK_VERIFY(vmaCreateImage(allocator, &depth_image_create_info, &alloc_info, &depth_image.image, &depth_image.allocation, null));

		const VkImageViewCreateInfo depth_image_view_create_info = vkinit::make_image_view_create_info(depth_image.format, depth_image.image, VK_IMAGE_ASPECT_DEPTH_BIT);

		VK_VERIFY(vkCreateImageView(device, &depth_image_view_create_info, null, &depth_image.image_view));

		// Add to deletion queue.
		deletion_queue.enqueue([this] {
			vkDestroyImageView(device, depth_image.image_view, null);
			vmaDestroyImage(allocator, depth_image.image, depth_image.allocation);

			vkDestroyImageView(device, draw_image.image_view, null);
			vmaDestroyImage(allocator, draw_image.image, draw_image.allocation);
		});
	}

	auto create_swapchain(const u32 width, const u32 height) -> void {
		ASSERT(!swapchain);

		// Create swapchain.
		vkb::SwapchainBuilder swapchain_builder{physical_device, device, surface};

		swapchain_image_format = VK_FORMAT_B8G8R8A8_UNORM;

		vkb::Swapchain vkb_swapchain = swapchain_builder
			.set_desired_format(VkSurfaceFormatKHR{
				.format = swapchain_image_format,
				.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
			})
			.set_desired_present_mode(VK_PRESENT_MODE_MAILBOX_KHR)
			.set_desired_extent(width, height)
			.add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
			.build()
			.value();

		swapchain_extent = vkb_swapchain.extent;
		swapchain = vkb_swapchain.swapchain;
		swapchain_images = vkb_swapchain.get_images().value();
		swapchain_image_views = vkb_swapchain.get_image_views().value();
	}

	auto destroy_swapchain() -> void {
		ASSERT(swapchain);

		vkDestroySwapchainKHR(device, swapchain, null);// @TODO: Implement allocator.

		swapchain = null;

		for (const auto swapchain_image_view : swapchain_image_views) {
			vkDestroyImageView(device, swapchain_image_view, null);
		}

		swapchain_image_views.clear();
	}

	auto init_commands() -> void {
		// Create a command pool for commands submitted to the graphics queue.
		// We also want the pool to allow for resetting of individual command buffers.
		const VkCommandPoolCreateInfo pool_create_info{
		  .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			.queueFamilyIndex = graphics_queue_family,
		};

		const VkFenceCreateInfo fence_create_info{
			.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
			.flags = VK_FENCE_CREATE_SIGNALED_BIT,// Start signaled so we don't wait on the first frame.
		};

		const VkSemaphoreCreateInfo semaphore_create_info{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		};

		for (i32 i = 0; i < FRAMES_IN_FLIGHT_COUNT; ++i) {
			VK_VERIFY(vkCreateCommandPool(device, &pool_create_info, null, &frames[i].command_pool));

			// Allocate the default command buffer that we will use for rendering.
			const VkCommandBufferAllocateInfo cmd_alloc_info{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandPool = frames[i].command_pool,
				.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				.commandBufferCount = 1,
			};

			VK_VERIFY(vkAllocateCommandBuffers(device, &cmd_alloc_info, &frames[i].command_buffer));

			// Init sync structures.
			VK_VERIFY(vkCreateFence(device, &fence_create_info, null, &frames[i].render_fence));
			VK_VERIFY(vkCreateSemaphore(device, &semaphore_create_info, null, &frames[i].render_semaphore));
			VK_VERIFY(vkCreateSemaphore(device, &semaphore_create_info, null, &frames[i].swapchain_semaphore));
		}

		// Init immediate command stuff.
		VK_VERIFY(vkCreateCommandPool(device, &pool_create_info, null, &immediate_command_pool));

		const VkCommandBufferAllocateInfo immediate_cmd_alloc_info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = immediate_command_pool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};

		VK_VERIFY(vkAllocateCommandBuffers(device, &immediate_cmd_alloc_info, &immediate_command_buffer));

		deletion_queue.enqueue([this] {
			ASSERT(immediate_command_pool);
			vkDestroyCommandPool(device, immediate_command_pool, null);
			immediate_command_pool = null;
		});

		// Init immediate syn structures.
		VK_VERIFY(vkCreateFence(device, &fence_create_info, null, &immediate_fence))

		deletion_queue.enqueue([this] {
			ASSERT(immediate_fence);
			vkDestroyFence(device, immediate_fence, null);
			immediate_fence = null;
		});
	}

	auto init_scene_data_descriptor() -> void {
		DescriptorLayoutBuilder builder;
		scene_data_descriptor_layout = builder
			.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
			.build(device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);// Accessible in both vertex and fragment shaders.

		deletion_queue.enqueue([this] {
			vkDestroyDescriptorSetLayout(device, scene_data_descriptor_layout, null);
		});
	}

	auto init_single_image_descriptor() -> void {
		DescriptorLayoutBuilder builder;
		single_image_descriptor_layout = builder
			.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
			.build(device, VK_SHADER_STAGE_FRAGMENT_BIT);

		deletion_queue.enqueue([this] {
			vkDestroyDescriptorSetLayout(device, single_image_descriptor_layout, null);
		});
	}

	auto init_descriptors() -> void {
		const DescriptorAllocator::PoolSizeRatio sizes[] = {
			DescriptorAllocator::PoolSizeRatio{
				.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
				.ratio = 1,
			},
		};

		descriptor_allocator.init_pool(device, 10, sizes);

		DescriptorLayoutBuilder layout_builder;
		layout_builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);

		draw_image_descriptor_layout = layout_builder.build(device, VK_SHADER_STAGE_COMPUTE_BIT);

		draw_image_descriptors = descriptor_allocator.allocate(device, draw_image_descriptor_layout);

#if 0
		const VkDescriptorImageInfo image_info{
			.imageView = draw_image.image_view,
			.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
		};

		const VkWriteDescriptorSet draw_image_write{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = draw_image_descriptors,
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			.pImageInfo = &image_info,
		};

		vkUpdateDescriptorSets(device, 1, &draw_image_write, 0, null);
#else
		DescriptorWriter writer;
		writer.write_image(0, draw_image.image_view, null, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		writer.update_set(device, draw_image_descriptors);
#endif

		deletion_queue.enqueue([this] {
			descriptor_allocator.destroy_pool(device);

			vkDestroyDescriptorSetLayout(device, draw_image_descriptor_layout, null);
		});

		for (i32 i = 0; i < FRAMES_IN_FLIGHT_COUNT; ++i) {
			// Create a descriptor pool.
			DescriptorAllocatorGrowable::PoolSizeRatio frame_sizes[] = {
				{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 },
				{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 },
				{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 },
				{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 },
			};

			frames[i].descriptors.init(device, 1000, frame_sizes);

			deletion_queue.enqueue([this, i] {
				frames[i].descriptors.destroy_pools(device);
			});
		}

		init_scene_data_descriptor();
		init_single_image_descriptor();
	}

	auto init_pipelines() -> void {
		init_background_pipelines();
		init_mesh_pipeline();
	}

	auto init_background_pipelines() -> void {
		const VkPushConstantRange push_constants{
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			.offset = 0,
			.size = sizeof(ComputePushConstants),
		};

#if 0
		const VkPipelineLayoutCreateInfo comp_layout_create_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &draw_image_descriptor_layout,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &push_constants,
		};

		VK_VERIFY(vkCreatePipelineLayout(device, &comp_layout_create_info, null, &gradient_pipeline_layout));
		const VkShaderModule background_shader = vkutils::load_shader_module(SHADER_PATH("gradient_color.comp"), device);

		const VkComputePipelineCreateInfo comp_pipeline_create_info{
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_COMPUTE_BIT,
				.module = background_shader,
				.pName = "main",
			},
			.layout = gradient_pipeline_layout,
		};

		VK_VERIFY(vkCreateComputePipelines(device, null, 1, &comp_pipeline_create_info, null, &gradient_pipeline));

		// The ShaderModule is only needed to create the pipeline. It is no longer needed so can safely just destroy it.
		vkDestroyShaderModule(device, background_shader, null);

		deletion_queue.enqueue([this] {
			vkDestroyPipelineLayout(device, gradient_pipeline_layout, null);
			vkDestroyPipeline(device, gradient_pipeline, null);
		});
#endif

		const auto add_comp_effect = [&](const char* shader_path) {
			const VkPipelineLayoutCreateInfo comp_layout_create_info{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.setLayoutCount = 1,
				.pSetLayouts = &draw_image_descriptor_layout,
				.pushConstantRangeCount = 1,
				.pPushConstantRanges = &push_constants,
			};

			VkPipelineLayout pipeline_layout;
			VK_VERIFY(vkCreatePipelineLayout(device, &comp_layout_create_info, null, &pipeline_layout));

			const VkShaderModule shader = vkutils::load_shader_module(shader_path, device);

			const VkComputePipelineCreateInfo comp_pipeline_create_info{
				.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
				.stage{
					.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
					.stage = VK_SHADER_STAGE_COMPUTE_BIT,
					.module = shader,
					.pName = "main",
				},
				.layout = pipeline_layout,
			};

			VkPipeline pipeline;
			VK_VERIFY(vkCreateComputePipelines(device, null, 1, &comp_pipeline_create_info, null, &pipeline));

			// No longer needed.
			vkDestroyShaderModule(device, shader, null);

			// Add to the array of background_effects.
			background_effects.push_back(ComputeEffect{
				.name = shader_path,
				.pipeline = pipeline,
				.layout = pipeline_layout,
				.data{
					.data1{1, 0, 0, 1},
					.data2{0, 0, 1, 1},
				},
			});

			deletion_queue.enqueue([this, pipeline, pipeline_layout] {
				vkDestroyPipelineLayout(device, pipeline_layout, null);
				vkDestroyPipeline(device, pipeline, null);
			});
		};

		add_comp_effect(SHADER_PATH("gradient_color.comp"));
		add_comp_effect(SHADER_PATH("sky.comp"));
	}

	auto init_imgui() -> void {
		// Create a descriptor pool for ImGUI.
		const VkDescriptorPoolSize pool_sizes[] = {
			{ VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
			{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
			{ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
		};

		const VkDescriptorPoolCreateInfo desc_pool_create_info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
			.maxSets = 1000,
			.poolSizeCount = static_cast<u32>(std::size(pool_sizes)),
			.pPoolSizes = pool_sizes,
		};

		VkDescriptorPool imgui_pool;
		VK_VERIFY(vkCreateDescriptorPool(device, &desc_pool_create_info, null, &imgui_pool));

		// Initializes core of ImGui.
		ImGui::CreateContext();

		ImGui_ImplSDL2_InitForVulkan(window);

		ImGui_ImplVulkan_InitInfo imgui_init_info{
			.Instance = instance,
			.PhysicalDevice = physical_device,
			.Device = device,
			.Queue = graphics_queue,
			.DescriptorPool = imgui_pool,
			.MinImageCount = 3,
			.ImageCount = 3,
			.MSAASamples = VK_SAMPLE_COUNT_1_BIT,
			.UseDynamicRendering = true,
			.PipelineRenderingCreateInfo{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
				.colorAttachmentCount = 1,
				.pColorAttachmentFormats = &swapchain_image_format,
			},
		};

		ImGui_ImplVulkan_Init(&imgui_init_info);

		ImGui_ImplVulkan_CreateFontsTexture();

		deletion_queue.enqueue([this, imgui_pool] {
			ImGui_ImplVulkan_Shutdown();
			vkDestroyDescriptorPool(device, imgui_pool, null);
		});
	}

	[[nodiscard]] auto create_image(const VkExtent3D& extent, const VkFormat format, const VkImageUsageFlags usage, const bool mipmapped = false) -> AllocatedImage {
		AllocatedImage new_image{
			.extent = extent,
			.format = format,
		};

		auto image_create_info = vkinit::make_image_create_info(format, usage, extent);
		if (mipmapped) {// Automatically determine number of mip-levels to produce.
			image_create_info.mipLevels = static_cast<u32>(std::floor(std::log2(std::max(extent.width, extent.height)))) + 1;
		}

		// Always allocate images on dedicated GPU memory.
		const VmaAllocationCreateInfo alloc_create_info{
			.usage = VMA_MEMORY_USAGE_GPU_ONLY,
			.requiredFlags = static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
		};

		VK_VERIFY(vmaCreateImage(allocator, &image_create_info, &alloc_create_info, &new_image.image, &new_image.allocation, null));

		// If it's a depth format need to make sure it has the correct aspect flags.
		const VkImageAspectFlags aspect_flags = format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

		// Build an image-view for the image.
		auto image_view_create_info = vkinit::make_image_view_create_info(format, new_image.image, aspect_flags);
		image_view_create_info.subresourceRange.levelCount = image_create_info.mipLevels;

		VK_VERIFY(vkCreateImageView(device, &image_view_create_info, null, &new_image.image_view));

		return new_image;
	}

	[[nodiscard]] auto create_image(const void* data, const VkExtent3D& extent, const VkFormat format, const VkImageUsageFlags usage, const bool mipmapped = false) -> AllocatedImage {
		const usize buffer_size = extent.width * extent.height * extent.depth * 4;// @NOTE: Assumes RGBA 8 format.
		const auto staging_buffer = create_buffer(buffer_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

		memcpy(staging_buffer.allocation->GetMappedData(), data, buffer_size);

		const auto new_image = create_image(extent, format, usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, mipmapped);

		immediate_submit([&](const VkCommandBuffer cmd) {
			// Transition the new image's layout to a transfer destination.
			vkutils::transition_image_layout(cmd, new_image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

			const VkBufferImageCopy copy_region{
				.bufferOffset = 0,
				.bufferRowLength = 0,
				.bufferImageHeight = 0,

				.imageSubresource{
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.mipLevel = 0,
					.baseArrayLayer = 0,
					.layerCount = 1,
				},

				.imageExtent = extent,
			};

			vkCmdCopyBufferToImage(cmd, staging_buffer.buffer, new_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);

			// Now transition the image layout to be optimally read by shaders.
			vkutils::transition_image_layout(cmd, new_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		});

		// No longer need the staging buffer. Can safely destroy it now.
		destroy_buffer(staging_buffer);

		return new_image;
	}

	[[nodiscard]] auto create_buffer(const usize size, const VkBufferUsageFlags usage, const VmaMemoryUsage memory_usage) -> AllocatedBuffer {
		const VkBufferCreateInfo buffer_create_info{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = size,
			.usage = usage,
		};

		const VmaAllocationCreateInfo alloc_create_info{
			.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
			.usage = memory_usage,
		};

		AllocatedBuffer out;
		VK_VERIFY(vmaCreateBuffer(allocator, &buffer_create_info, &alloc_create_info, &out.buffer, &out.allocation, &out.allocation_info));

		return out;
	}

	auto destroy_image(const AllocatedImage& image) -> void {
		vkDestroyImageView(device, image.image_view, null);
		vmaDestroyImage(allocator, image.image, image.allocation);
	}

	auto destroy_buffer(const AllocatedBuffer& buffer) -> void {
		vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
	}

	[[nodiscard]] auto upload_mesh(const Span<const u32> indices, const Span<const Vertex> vertices) -> GpuMeshBuffers {
		GpuMeshBuffers out;

		const auto index_buffer_size = indices.size() * sizeof(u32);
		const auto vertex_buffer_size = vertices.size() * sizeof(Vertex);

		// Create vertex buffer.
		out.vertex_buffer = create_buffer(vertex_buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

		const VkBufferDeviceAddressInfo device_address_info{
			.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			.buffer = out.vertex_buffer.buffer,
		};

		out.vertex_buffer_address = vkGetBufferDeviceAddress(device, &device_address_info);

		// Create index buffer.
		out.index_buffer = create_buffer(index_buffer_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

		const AllocatedBuffer staging_buffer = create_buffer(vertex_buffer_size + index_buffer_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);

		void* staging_data = staging_buffer.allocation->GetMappedData();

		// Copy vertex buffer.
		memcpy(staging_data, vertices.data(), vertex_buffer_size);

		// Copy index buffer.
		memcpy(reinterpret_cast<char*>(staging_data) + vertex_buffer_size, indices.data(), index_buffer_size);

		static constexpr auto THING = cpts::Invokable<decltype([](VkCommandBuffer) {}), VkCommandBuffer>;

		immediate_submit([&](VkCommandBuffer cmd) {
			const VkBufferCopy vertices_copy{
				.srcOffset = 0,
				.dstOffset = 0,
				.size = vertex_buffer_size,
			};

			vkCmdCopyBuffer(cmd, staging_buffer.buffer, out.vertex_buffer.buffer, 1, &vertices_copy);

			const VkBufferCopy indices_copy{
				.srcOffset = vertex_buffer_size,
				.dstOffset = 0,
				.size = index_buffer_size,
			};

			vkCmdCopyBuffer(cmd, staging_buffer.buffer, out.index_buffer.buffer, 1, &indices_copy);
		});

		// No longer needed.
		destroy_buffer(staging_buffer);

		return out;
	}

	auto immediate_submit(const FnRef<void(VkCommandBuffer cmd)>& fn) -> void {
		VK_VERIFY(vkResetFences(device, 1, &immediate_fence));
		VK_VERIFY(vkResetCommandBuffer(immediate_command_buffer, 0));

		VkCommandBuffer cmd = immediate_command_buffer;
		ASSERT(cmd);

		const VkCommandBufferBeginInfo cmd_begin_info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};

		VK_VERIFY(vkBeginCommandBuffer(cmd, &cmd_begin_info));

		fn(cmd);

		VK_VERIFY(vkEndCommandBuffer(cmd));

		const VkCommandBufferSubmitInfo cmd_submit_info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
			.commandBuffer = cmd,
		};

		const VkSubmitInfo2 submit_info{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
			.commandBufferInfoCount = 1,
			.pCommandBufferInfos = &cmd_submit_info,
		};

		// Submit command buffer to the queue and execute it. Block CPU until graphics command has finished execution.
		// @NOTE: Suboptimal.
		VK_VERIFY(vkQueueSubmit2(graphics_queue, 1, &submit_info, immediate_fence));

		VK_VERIFY(vkWaitForFences(device, 1, &immediate_fence, true, UINT64_MAX));
	}

	auto draw_background(VkCommandBuffer cmd) -> void {
		VkClearColorValue clear_color;
		const auto flash = std::abs(std::sin(static_cast<f32>(current_frame_count) / 120.f));
		clear_color = {{0.f, 0.f, flash, 1.f}};

		// All mips.
		const VkImageSubresourceRange clear_range{
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = VK_REMAINING_MIP_LEVELS,
			.baseArrayLayer = 0,
			.layerCount = VK_REMAINING_ARRAY_LAYERS,
		};

		// Clear the image.
		//vkCmdClearColorImage(cmd, draw_image.image, VK_IMAGE_LAYOUT_GENERAL, &clear_color, 1, &clear_range);

		// Bind the gradient drawing compute pipeline.
#if 0
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gradient_pipeline);

		// Bind the descriptor set containing the draw image for the compute pipeline.
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gradient_pipeline_layout, 0, 1, &draw_image_descriptors, 0, null);

		// Bind push constants.
		const ComputePushConstants push_constants{
			.data1{1, 0, 0, 1},
			.data2{0, 0, 1, 0},
		};

		vkCmdPushConstants(cmd, gradient_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &push_constants);
#else
		const auto& selected_background = background_effects[current_background_effect];
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, selected_background.pipeline);

		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, selected_background.layout, 0, 1, &draw_image_descriptors, 0, null);

		vkCmdPushConstants(cmd, selected_background.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &selected_background.data);
#endif
		// Execute the compute pipeline dispatch. We are using 16x16 workgroup size so divide by that.
		vkCmdDispatch(cmd, std::ceil(static_cast<f32>(draw_extent.width) / 16), std::ceil(static_cast<f32>(draw_extent.height) / 16), 1);
	}

	auto init_mesh_pipeline() -> void {
		//const VkShaderModule mesh_frag_shader = vkutils::load_shader_module(SHADER_PATH("colored_triangle.frag"), device);
		const VkShaderModule mesh_frag_shader = vkutils::load_shader_module(SHADER_PATH("tex_image.frag"), device);
		const VkShaderModule mesh_vert_shader = vkutils::load_shader_module(SHADER_PATH("colored_triangle_mesh.vert"), device);

		const VkPushConstantRange push_constant{
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
			.offset = 0,
			.size = sizeof(GpuDrawPushConstants),
		};

		DescriptorLayoutBuilder layout_builder;
		auto mesh_descriptor_layout = layout_builder
			.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
			.build(device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

		deletion_queue.enqueue([this, mesh_descriptor_layout] {
			vkDestroyDescriptorSetLayout(device, mesh_descriptor_layout, null);
		});

		const VkPipelineLayoutCreateInfo pipeline_layout_create_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &single_image_descriptor_layout,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &push_constant,
		};

		VK_VERIFY(vkCreatePipelineLayout(device, &pipeline_layout_create_info, null, &mesh_pipeline_layout));

		vkutils::PipelineBuilder pipeline_builder;

		pipeline_builder.pipeline_layout = mesh_pipeline_layout;

		mesh_pipeline = pipeline_builder
			.set_shaders(mesh_vert_shader, mesh_frag_shader)
			.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
			.set_polygon_mode(VK_POLYGON_MODE_FILL)
			.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE)
			.disable_msaa()
			.disable_blending()
			//.enable_blending_additive()
			.enable_depth_test(true, VK_COMPARE_OP_GREATER_OR_EQUAL)
			.set_color_attachment_format(draw_image.format)
			.set_depth_attachment_format(depth_image.format)
			.build(device);

		// No longer needed.
		vkDestroyShaderModule(device, mesh_frag_shader, null);
		vkDestroyShaderModule(device, mesh_vert_shader, null);

		deletion_queue.enqueue([this] {
			vkDestroyPipelineLayout(device, mesh_pipeline_layout, null);
			vkDestroyPipeline(device, mesh_pipeline, null);
		});
	}

	auto resize_swapchain() -> void {
		ASSERT(resize_requested);

		vkDeviceWaitIdle(device);

		destroy_swapchain();

		i32 width, height;
		SDL_GetWindowSize(window, &width, &height);

		create_swapchain(width, height);

		resize_requested = false;
	}

	auto draw_imgui(VkCommandBuffer cmd, VkImageView target_image_view) -> void {
		const VkRenderingAttachmentInfo color_attachment = vkinit::make_attachment_info(target_image_view, NULL_OPTIONAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		const VkRenderingInfo render_info{
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.renderArea{
				.offset{0, 0},
				.extent = swapchain_extent,
			},
			.layerCount = 1,
			.colorAttachmentCount = 1,
			.pColorAttachments = &color_attachment,

			// Unused for now.
			.pDepthAttachment = null,
			.pStencilAttachment = null,
		};

		vkCmdBeginRendering(cmd, &render_info);

		ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

		vkCmdEndRendering(cmd);
	}

	auto init_default_data() -> void {
		auto loaded_meshes = load_gltf_meshes(ASSET_PATH("basicmesh.glb"));
		ASSERT(loaded_meshes);

		test_meshes = std::move(*loaded_meshes);

		// Init default textures and samplers.

		// 3 default textures: White, Gray, Black. 1 pixel each.
		const u32 white = glm::packUnorm4x8(glm::vec4{1.f, 1.f, 1.f, 1.f});
		const u32 gray = glm::packUnorm4x8(glm::vec4{0.67f, 0.67f, 0.67f, 1.f});
		const u32 black = glm::packUnorm4x8(glm::vec4{0.f, 0.f, 0.f, 0.f});

		// Checkerboard image.
		const u32 magenta = glm::packUnorm4x8(glm::vec4{1.f, 0.f, 1.f, 1.f});

		// 16x16 texture.
		// @NOTE: Probably unnecessary to even have this resolution. 2x2 may suffice.
		u32 checkerboard_pixels[math::square(16)];
		for (i32 x = 0; x < 16; ++x) {
			for (i32 y = 0; y < 16; ++y) {
				checkerboard_pixels[x + y * 16] = ((x % 2) ^ (y % 2)) ? magenta : black;
			}
		}

		// Create images.
		white_image = create_image(&white, VkExtent3D{1, 1, 1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
		gray_image = create_image(&gray, VkExtent3D{1, 1, 1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
		black_image = create_image(&black, VkExtent3D{1, 1, 1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
		error_checkerboard_image = create_image(checkerboard_pixels, VkExtent3D{16, 16, 1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);

		// Create samplers.
		const VkSamplerCreateInfo linear_sampler_create_info{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_LINEAR,
			.minFilter = VK_FILTER_LINEAR,
		};

		const VkSamplerCreateInfo nearest_sampler_create_info{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_NEAREST,
			.minFilter = VK_FILTER_NEAREST,
		};

		VK_VERIFY(vkCreateSampler(device, &linear_sampler_create_info, null, &default_sampler_linear));
		VK_VERIFY(vkCreateSampler(device, &nearest_sampler_create_info, null, &default_sampler_nearest));

		deletion_queue.enqueue([this] {
			vkDestroySampler(device, default_sampler_linear, null);
			vkDestroySampler(device, default_sampler_nearest, null);

			destroy_image(white_image);
			destroy_image(gray_image);
			destroy_image(black_image);
			destroy_image(error_checkerboard_image);
		});
	}

	[[nodiscard]] auto load_gltf_meshes(const std::filesystem::path& file_path) -> Optional<Array<SharedPtr<MeshAsset>>> {
		return vkutils::load_gltf_meshes(file_path, [&](MeshAsset& mesh, const Span<const u32> indices, const Span<const Vertex> vertices) {
			mesh.mesh_buffers = upload_mesh(indices, vertices);
		});
	}

	auto draw_geometry(VkCommandBuffer cmd) -> void {
		const VkRenderingAttachmentInfo color_attachment = vkinit::make_attachment_info(draw_image.image_view, NULL_OPTIONAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		const VkRenderingAttachmentInfo depth_attachment = vkinit::make_depth_attachment_info(depth_image.image_view, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

		const VkRenderingInfo render_info{
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.renderArea{
				.offset{0, 0},
				.extent = draw_extent,
			},
			.layerCount = 1,
			.colorAttachmentCount = 1,
			.pColorAttachments = &color_attachment,
			.pDepthAttachment = &depth_attachment,

			// Unused for now.
			.pStencilAttachment = null,
		};

		vkCmdBeginRendering(cmd, &render_info);

		// Set dynamic viewport and scissor.
		const VkViewport viewport{
			.x = 0,
			.y = 0,
			.width = static_cast<f32>(draw_extent.width),
			.height = static_cast<f32>(draw_extent.height),
			.minDepth = 0.f,
			.maxDepth = 1.f,
		};

		const VkRect2D scissor{
			.offset{0, 0},
			.extent = draw_extent,
		};

		vkCmdSetViewport(cmd, 0, 1, &viewport);
		vkCmdSetScissor(cmd, 0, 1, &scissor);

		// Allocate a new buffer for scene data.
		// @NOTE: VMA_MEMORY_USAGE_CPU_TO_GPU is marked deprecated for some reason but I don't know what the alternative is.
		const AllocatedBuffer scene_data_buffer = create_buffer(sizeof(GpuSceneData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

		get_current_frame_data().deletion_queue.enqueue([this, scene_data_buffer] {
			destroy_buffer(scene_data_buffer);
		});

		static_assert(std::is_trivial_v<GpuSceneData>);

		// Write to scene_data_buffer.
		auto* scene_data_allocation = static_cast<GpuSceneData*>(scene_data_buffer.allocation->GetMappedData());
		std::construct_at(scene_data_allocation, scene_data);

		// Create a descriptor set that binds that buffer and update it. (Will be automatically freed when this frame index is used again).
		const auto global_descriptor_set = get_current_frame_data().descriptors.allocate(device, scene_data_descriptor_layout);

		DescriptorWriter desc_writer;
		desc_writer.write_buffer(0, scene_data_buffer.buffer, sizeof(GpuSceneData), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
		desc_writer.update_set(device, global_descriptor_set);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline);

		// Bind a texture.
		const auto image_set = get_current_frame_data().descriptors.allocate(device, single_image_descriptor_layout);
		{
			DescriptorWriter writer;
			writer.write_image(0, error_checkerboard_image.image_view, default_sampler_nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			writer.update_set(device, image_set);
		}

		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline_layout, 0, 1, &image_set, 0, null);

		const auto& mesh = *test_meshes[2];

		glm::mat4 view = glm::translate(glm::vec3{0.f, 0.f, -5.f});

		const glm::mat4 model_to_world = glm::rotate(glm::radians((static_cast<f32>(current_frame_count) / 120.f) * 3.14f), glm::vec3{0.f, 0.f, 1.f});

		// Camera projection.
		glm::mat4 projection = glm::perspective(glm::radians(90.f), static_cast<float>(draw_extent.width) / static_cast<float>(draw_extent.height), 10000.f, 0.1f);

		// Invert the Y direction on projection matrix so that we are similar to OpenGL and gltf axis.
		projection[1][1] *= -1.f;

		const GpuDrawPushConstants push_constants{
			.render_matrix = projection * model_to_world * view,
			.vertex_buffer = mesh.mesh_buffers.vertex_buffer_address,
		};

		vkCmdPushConstants(cmd, mesh_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GpuDrawPushConstants), &push_constants);
		vkCmdBindIndexBuffer(cmd, mesh.mesh_buffers.index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);

		vkCmdDrawIndexed(cmd, mesh.surfaces[0].count, 1, mesh.surfaces[0].start_index, 0, 0);

		vkCmdEndRendering(cmd);
	}

	auto draw() -> void {
		auto& current_frame = get_current_frame_data();

		// Wait until the GPU has finished rendering the previous frame before attempting to draw the next frame.
		VK_VERIFY(vkWaitForFences(device, 1, &current_frame.render_fence, true, UINT64_MAX));
		VK_VERIFY(vkResetFences(device, 1, &current_frame.render_fence));// Reset after waiting.

		// Flush resources pending deletion after previous-frame has finished presenting.
		current_frame.deletion_queue.flush();
		current_frame.descriptors.clear_pools(device);

		u32 swapchain_image_index;
		const VkResult acquire_next_image_result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, current_frame.swapchain_semaphore, null, &swapchain_image_index);
		if (acquire_next_image_result == VK_ERROR_OUT_OF_DATE_KHR) {
			resize_requested = true;
			return;
		} else if (acquire_next_image_result != VK_SUCCESS) {
			ASSERT_UNREACHABLE;
		}
		//VK_VERIFY(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, current_frame.swapchain_semaphore, null, &swapchain_image_index));

		// Reset command buffer.
		VkCommandBuffer cmd = current_frame.command_buffer;

		VK_VERIFY(vkResetCommandBuffer(cmd, 0));

		// Begin "render pass".
		const VkCommandBufferBeginInfo cmd_begin_info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};

#if 0
		draw_extent.width = draw_image.extent.width;
		draw_extent.height = draw_image.extent.height;
#else
		draw_extent.width = std::min(draw_image.extent.width, swapchain_extent.width) * render_scale;
		draw_extent.height = std::min(draw_image.extent.height, swapchain_extent.height) * render_scale;
#endif

		VK_VERIFY(vkBeginCommandBuffer(cmd, &cmd_begin_info));

		// Make the swapchain image writeable before rendering to it.
		//vkutils::transition_image_layout(cmd, swapchain_images[swapchain_image_index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

		vkutils::transition_image_layout(cmd, draw_image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

		draw_background(cmd);

		// Geometry must be drawn with the image layout VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL (otherwise performance will be degraded).
		vkutils::transition_image_layout(cmd, draw_image.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		vkutils::transition_image_layout(cmd, depth_image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

		draw_geometry(cmd);

		// Transition to draw image to VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL in preparation for copying the image to the presenting swapchain.
		vkutils::transition_image_layout(cmd, draw_image.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		vkutils::transition_image_layout(cmd, swapchain_images[swapchain_image_index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		vkutils::copy_image_to_image(cmd, draw_image.image, swapchain_images[swapchain_image_index], draw_extent, swapchain_extent);

		//vkutils::transition_image_layout(cmd, swapchain_images[swapchain_image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

		// Set swapchain image layout to Attachment Optimal so we can draw to it.
		vkutils::transition_image_layout(cmd, swapchain_images[swapchain_image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

		// Draw ImGui directly onto the swapchain image.
		draw_imgui(cmd, swapchain_image_views[swapchain_image_index]);

		// Transition swapchain layout to a presentable layout so it can be displayed.
		vkutils::transition_image_layout(cmd, swapchain_images[swapchain_image_index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

		// Finalize the command buffer.
		VK_VERIFY(vkEndCommandBuffer(cmd));

		// Submit.
		const VkCommandBufferSubmitInfo cmd_submit_info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
			.commandBuffer = cmd,
		};

		const VkSemaphoreSubmitInfo wait_info{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			.semaphore = current_frame.swapchain_semaphore,
			.value = 1,
			.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR,
			.deviceIndex = 0,
		};

		const VkSemaphoreSubmitInfo signal_info{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			.semaphore = current_frame.render_semaphore,
			.value = 1,
			.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
			.deviceIndex = 0,
		};

		const VkSubmitInfo2 submit_info{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
			.waitSemaphoreInfoCount = 1,
			.pWaitSemaphoreInfos = &wait_info,
			.commandBufferInfoCount = 1,
			.pCommandBufferInfos = &cmd_submit_info,
			.signalSemaphoreInfoCount = 1,
			.pSignalSemaphoreInfos = &signal_info,
		};

		VK_VERIFY(vkQueueSubmit2(graphics_queue, 1, &submit_info, current_frame.render_fence));

		// Present the image to the screen.
		const VkPresentInfoKHR present_info{
			.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,

			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &current_frame.render_semaphore,

			.swapchainCount = 1,
			.pSwapchains = &swapchain,

			.pImageIndices = &swapchain_image_index,
		};

		//VK_VERIFY(vkQueuePresentKHR(graphics_queue, &present_info));
		const VkResult present_result = vkQueuePresentKHR(graphics_queue, &present_info);
		if (present_result == VK_ERROR_OUT_OF_DATE_KHR) {
			resize_requested = true;
			return;
		} else if (present_result != VK_SUCCESS) {
			ASSERT_UNREACHABLE;
		}

		++current_frame_count;
	}

	auto destroy_resources() -> void {
		// Wait for the GPU to stop using these resources before freeing them.
		vkDeviceWaitIdle(device);

		// Destroy frame-data.
		for (i32 i = 0; i < FRAMES_IN_FLIGHT_COUNT; ++i) {
			// Destroy per-frame allocations.
			frames[i].deletion_queue.flush();

			// Destroy command pool.
			vkDestroyCommandPool(device, frames[i].command_pool, null);

			// Destroy synchronization structures.
			vkDestroyFence(device, frames[i].render_fence, null);
			vkDestroySemaphore(device, frames[i].render_semaphore, null);
			vkDestroySemaphore(device, frames[i].swapchain_semaphore, null);

			frames[i].command_pool = null;
			frames[i].render_semaphore = null;
			frames[i].swapchain_semaphore = null;
		}

		// Destroy mesh asset resources.
		for (auto& mesh : test_meshes) {
			destroy_buffer(mesh->mesh_buffers.index_buffer);
			destroy_buffer(mesh->mesh_buffers.vertex_buffer);
		}

		// Flush global deletion-queue.
		deletion_queue.flush();

		destroy_swapchain();

		vkDestroySurfaceKHR(instance, surface, null);
		vkDestroyDevice(device, null);

		vkb::destroy_debug_utils_messenger(instance, debug_messenger);
		vkDestroyInstance(instance, null);

		// NULL out references. Shouldn't necessarily be required.
		instance = null;
		device = null;
		surface = null;

		SDL_Quit();

		ASSERT(window);
		SDL_DestroyWindow(window);
		window = null;
	}

	SDL_Window* window = null;

	VkInstance instance = null;
	VkDebugUtilsMessengerEXT debug_messenger = null;// Debug output handle.
	VkPhysicalDevice physical_device = null;
	VkDevice device = null;
	VkSurfaceKHR surface = null;

	VkSwapchainKHR swapchain = null;
	VkFormat swapchain_image_format;

	Array<VkImage> swapchain_images;
	Array<VkImageView> swapchain_image_views;
	VkExtent2D swapchain_extent;

	FrameData frames[FRAMES_IN_FLIGHT_COUNT];

	usize current_frame_count = 0;

	VkQueue graphics_queue = null;
	u32 graphics_queue_family = std::numeric_limits<u32>::max();

	VmaAllocator allocator = null;

	// Draw resources.
	AllocatedImage draw_image;
	AllocatedImage depth_image;
	VkExtent2D draw_extent;
	f32 render_scale = 1.f;

	DescriptorAllocator descriptor_allocator;
	VkDescriptorSet draw_image_descriptors;
	VkDescriptorSetLayout draw_image_descriptor_layout;

	GpuSceneData scene_data;
	VkDescriptorSetLayout scene_data_descriptor_layout;

	VkDescriptorSetLayout single_image_descriptor_layout;

	VkPipeline gradient_pipeline = null;
	VkPipelineLayout gradient_pipeline_layout = null;

	// Immediate submit structures.
	VkFence immediate_fence = null;
	VkCommandBuffer immediate_command_buffer = null;
	VkCommandPool immediate_command_pool = null;

	Array<ComputeEffect> background_effects;
	i32 current_background_effect = 0;

	VkPipelineLayout mesh_pipeline_layout;
	VkPipeline mesh_pipeline;

	Array<SharedPtr<MeshAsset>> test_meshes;

	AllocatedImage white_image;
	AllocatedImage black_image;
	AllocatedImage gray_image;
	AllocatedImage error_checkerboard_image;
	VkSampler default_sampler_linear;
	VkSampler default_sampler_nearest;

	bool resize_requested = false;

	DeletionQueue deletion_queue;
};
}

using res::VkEngine;
using res::IsRendering;
using res::RequestExit;

namespace vk {
struct InitSystem {
	constexpr explicit InitSystem(WindowConfig window_config)
		: window_config{std::move(window_config)} {}

	[[nodiscard]] static auto get_access_requirements() -> AccessRequirements {
		return {
			.resources{
				.writes = ResMask::make<res::VkEngine, res::IsRendering>(),
			},
		};
	}

	auto execute(ExecContext& context) -> void {
		SDL_Init(SDL_INIT_VIDEO);

		auto& engine = context.get_mut_res<res::VkEngine>();

		engine.init(window_config);

		// Begin rendering.
		context.get_mut_res<res::IsRendering>().value = true;
	}

	WindowConfig window_config;
};

auto draw(ExecContext& context, Res<VkEngine> engine, Res<IsRendering> is_rendering, Res<RequestExit> request_exit) -> void {
	// Handle SDL events.
	{
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			switch (event.type) {
			case SDL_QUIT: {
				request_exit->value = true;
				return;
			}
			case SDL_WINDOWEVENT: {
				switch (event.window.event) {
				case SDL_WINDOWEVENT_MINIMIZED:
					is_rendering->value = false;
					break;

				case SDL_WINDOWEVENT_RESTORED:
					is_rendering->value = true;
					break;
				}
			}
			}

			// Send events to ImGui to handle.
			ImGui_ImplSDL2_ProcessEvent(&event);
		}
	}

	if (!is_rendering->value) {// Window is minimized or something.
		std::this_thread::sleep_for(std::chrono::milliseconds{100});// @TODO: Implement actual frame-pacing.
		return;
	}

	if (engine->resize_requested) {
		engine->resize_swapchain();
		ASSERT(!engine->resize_requested);
	}

	// Draw for ImGui.
	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();

	if (ImGui::Begin("Some UI")) {
		const auto current_time_point = std::chrono::high_resolution_clock::now();
		static auto previous_time_point = current_time_point;

		auto& selected = engine->background_effects[engine->current_background_effect];

		ImGui::Text("Framerate ms: %.3f", static_cast<f32>(std::chrono::duration_cast<std::chrono::microseconds>(current_time_point - previous_time_point).count()) / 1000);

		ImGui::SliderFloat("Render Scale", &engine->render_scale, 0.3f, 1.f);

		ImGui::Text("Selected effect: %s", selected.name);

		ImGui::SliderInt("Effect Index: ", &engine->current_background_effect, 0, engine->background_effects.size() - 1);

		ImGui::InputFloat3("color", reinterpret_cast<float*>(&engine->scene_data.ambient_color));

		ImGui::InputFloat4("data1", reinterpret_cast<float*>(&selected.data.data1));
		ImGui::InputFloat4("data2", reinterpret_cast<float*>(&selected.data.data2));
		ImGui::InputFloat4("data3", reinterpret_cast<float*>(&selected.data.data3));
		ImGui::InputFloat4("data4", reinterpret_cast<float*>(&selected.data.data4));

		previous_time_point = current_time_point;
	}
	ImGui::End();

	ImGui::Render();

	// Draw.
	engine->draw();
}

auto shutdown(ExecContext& context, Res<VkEngine> engine, Res<IsRendering> is_rendering) -> void {
	is_rendering->value = false;

	engine->destroy_resources();
}
}

auto VkPlugin::init(App& app) -> void {
	app
		.register_resource<res::VkEngine>()
		.register_resource<res::IsRendering>()
		.register_group<group::RenderInit>(Ordering{
			.within = GroupId::invalid_id(),
		})
		.register_group<group::RenderFrame>(Ordering{
			.within = GroupId::invalid_id(),
			.after = GroupMask::make<group::Movement>(),
		})
		.register_group<group::RenderShutdown>(Ordering{
			.within = GroupId::invalid_id(),
			.after = GroupMask::make<group::RenderInit>(),
		})
		.register_system<vk::InitSystem>(SystemDesc{
			.group = get_group_id<group::RenderInit>(),
			.event = get_event_id<event::OnInit>(),
			.priority = Priority::HIGH,
			.thread = Thread::MAIN,// SDL must be initialized on the main thread.
		}, std::move(window_config))
		.register_system<vk::draw>(SystemDesc{
			.group = get_group_id<group::RenderFrame>(),
			.event = get_event_id<event::OnUpdate>(),
			.priority = Priority::HIGH,
			// NOTE: Command pool is only thread safe to write to on the thread it is created on. Should either create a RenderThread or create thread-local command pools.
			.thread = Thread::MAIN,
		})
		.register_system<vk::shutdown>(SystemDesc{
			.group = get_group_id<group::RenderShutdown>(),
			.event = get_event_id<event::OnShutdown>(),
			.priority = Priority::LOW,
			.thread = Thread::MAIN,
		});
}
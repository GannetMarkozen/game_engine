#include "vk_plugin.hpp"
#include <cstddef>
#include <fstream>

#define VMA_IMPLEMENTATION
#include <vma/vk_mem_alloc.h>
#include <SDL.h>
#include <SDL_vulkan.h>

#include <VkBootstrap.h>

#include "defines.hpp"
#include "ecs/app.hpp"
#include "gameplay_framework.hpp"
#include "vulkan/vulkan_core.h"
// @TMP: All of this is tmp until I can come up with better abstractions.

// Asserts if the command being executed does not result in VK_SUCCESS. Only checks in Debug builds.
#define VK_VERIFY(CMD) { \
		[[maybe_unused]] const VkResult vk_result = CMD; \
		ASSERTF(vk_result == VK_SUCCESS, "{} != VK_SUCCESS!", #CMD); \
	}

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

	Array<Fn<void()>> dtors;
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
		self.window = SDL_CreateWindow(window_config.title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, window_config.extent.width, window_config.extent.height, SDL_WINDOW_VULKAN);

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

		// Create swapchain.
		self.create_swapchain(window_config);

		// Get the graphics queue.
		self.graphics_queue = device.get_queue(vkb::QueueType::graphics).value();
		self.graphics_queue_family = device.get_queue_index(vkb::QueueType::graphics).value();

		// Init commands.
		self.init_commands();

		self.init_descriptors();
		self.init_pipelines();
	}

	auto create_swapchain(const WindowConfig& window_config) -> void {
		ASSERT(!swapchain);

		// Create swapchain.
		vkb::SwapchainBuilder swapchain_builder{physical_device, device, surface};

		swapchain_image_format = VK_FORMAT_B8G8R8A8_UNORM;

		vkb::Swapchain vkb_swapchain = swapchain_builder
			.set_desired_format(VkSurfaceFormatKHR{
				.format = swapchain_image_format,
				.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
			})
			.set_desired_present_mode(window_config.present_mode)
			.set_desired_extent(window_config.extent.width, window_config.extent.height)
			.add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
			.build()
			.value();

		swapchain = vkb_swapchain.swapchain;
		swapchain_images = vkb_swapchain.get_images().value();
		swapchain_image_views = vkb_swapchain.get_image_views().value();

		// Assign extents.
		swapchain_extent.width = window_config.extent.width;
		swapchain_extent.height = window_config.extent.height;

		// Create draw image.
		const VkExtent3D draw_extent_3d{
			static_cast<u32>(window_config.extent.width),
			static_cast<u32>(window_config.extent.height),
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

		// Add to deletion queue.
		deletion_queue.enqueue([this] {
			vkDestroyImageView(device, draw_image.image_view, null);
			vmaDestroyImage(allocator, draw_image.image, draw_image.allocation);
		});
	}

	auto destroy_swapchain() -> void {
		ASSERT(swapchain);

		vkDestroySwapchainKHR(device, swapchain, null);// @TODO: Implement allocator.

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

		deletion_queue.enqueue([this] {
			descriptor_allocator.destroy_pool(device);

			vkDestroyDescriptorSetLayout(device, draw_image_descriptor_layout, null);
		});
	}

	auto init_pipelines() -> void {
		init_background_pipelines();
	}

	auto init_background_pipelines() -> void {
		const VkPipelineLayoutCreateInfo comp_layout_create_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &draw_image_descriptor_layout,
		};

		VK_VERIFY(vkCreatePipelineLayout(device, &comp_layout_create_info, null, &gradient_pipeline_layout));

		const VkShaderModule background_shader = vkutils::load_shader_module(SHADER_PATH("gradient.spv"), device);

#if 0
		const VkPipelineShaderStageCreateInfo stage_create_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = background_shader,
			.pName = "main",
		};

		const VkComputePipelineCreateInfo comp_pipeline_create_info{
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = stage_create_info,
			.layout = gradient_pipeline_layout,
		};
#endif

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
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gradient_pipeline);

		// Bind the descriptor set containing the draw image for the compute pipeline.
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gradient_pipeline_layout, 0, 1, &draw_image_descriptors, 0, null);

		// Execute the compute pipeline dispatch. We are using 16x16 workgroup size so divide by that.
		vkCmdDispatch(cmd, std::ceil(static_cast<f32>(draw_extent.width) / 16), std::ceil(static_cast<f32>(draw_extent.height) / 16), 1);
	}

	auto draw() -> void {
		auto& current_frame = get_current_frame_data();

		// Wait until the GPU has finished rendering the previous frame before attempting to draw the next frame.
		VK_VERIFY(vkWaitForFences(device, 1, &current_frame.render_fence, true, UINT64_MAX));
		VK_VERIFY(vkResetFences(device, 1, &current_frame.render_fence));// Reset after waiting.

		// Flush resources pending deletion after previous-frame has finished presenting.
		current_frame.deletion_queue.flush();

		u32 swapchain_image_index;
		VK_VERIFY(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, current_frame.swapchain_semaphore, null, &swapchain_image_index));

		// Reset command buffer.
		VkCommandBuffer cmd = current_frame.command_buffer;

		VK_VERIFY(vkResetCommandBuffer(cmd, 0));

		// Begin "render pass".
		const VkCommandBufferBeginInfo cmd_begin_info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};

		draw_extent.width = draw_image.extent.width;
		draw_extent.height = draw_image.extent.height;

		VK_VERIFY(vkBeginCommandBuffer(cmd, &cmd_begin_info));

		// Make the swapchain image writeable before rendering to it.
		//vkutils::transition_image_layout(cmd, swapchain_images[swapchain_image_index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

		vkutils::transition_image_layout(cmd, draw_image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

		draw_background(cmd);

		vkutils::transition_image_layout(cmd, draw_image.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		vkutils::transition_image_layout(cmd, swapchain_images[swapchain_image_index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		vkutils::copy_image_to_image(cmd, draw_image.image, swapchain_images[swapchain_image_index], draw_extent, swapchain_extent);

		vkutils::transition_image_layout(cmd, swapchain_images[swapchain_image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

		// Make swapchain presentable.
		//vkutils::transition_image_layout(cmd, swapchain_images[swapchain_image_index], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

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

		VK_VERIFY(vkQueuePresentKHR(graphics_queue, &present_info));

		++current_frame_count;
	}

	auto destroy_resources() -> void {
		// Wait for the GPU to stop using these resources before freeing them.
		vkDeviceWaitIdle(device);

		// Destroy frame-data.
		for (i32 i = 0; i < FRAMES_IN_FLIGHT_COUNT; ++i) {
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
	VkExtent2D draw_extent;

	DescriptorAllocator descriptor_allocator;
	VkDescriptorSet draw_image_descriptors;
	VkDescriptorSetLayout draw_image_descriptor_layout;

	VkPipeline gradient_pipeline = null;
	VkPipelineLayout gradient_pipeline_layout = null;

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
		}
	}

	if (!is_rendering->value) {// Window is minimized or something.
		std::this_thread::sleep_for(std::chrono::milliseconds{100});// @TODO: Implement actual frame-pacing.
		return;
	}

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
#include "init.hpp"
#include "defines.hpp"
#include "gameplay_framework.hpp"
#include "renderer_plugin.hpp"
#include "vk_verify.hpp"

#include "SDL_video.h"
#include "vulkan/vulkan_core.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <VkBootstrap.h>
#include <cstdint>

namespace renderer {
static auto init_swapchain(GlobalResources& resources, GlobalDeletionQueue& deletion_queue, const usize width, const usize height) -> void {
	resources.swapchain = vkutils::create_swapchain(resources.device, resources.physical_device, resources.surface, width, height);// Other args defaulted.

	static constexpr auto DRAW_IMAGE_USAGE_FLAGS = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	resources.draw_image = vkutils::create_image(resources.device, resources.allocator, VkExtent2D{static_cast<u32>(width), static_cast<u32>(height)}, VK_FORMAT_R16G16B16A16_SFLOAT, DRAW_IMAGE_USAGE_FLAGS, false);

	static constexpr auto DEPTH_IMAGE_USAGE_FLAGS = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	resources.depth_image = vkutils::create_image(resources.device, resources.allocator, VkExtent2D{static_cast<u32>(width), static_cast<u32>(height)}, VK_FORMAT_D32_SFLOAT, DEPTH_IMAGE_USAGE_FLAGS, false);

	deletion_queue.enqueue([&resources] {
		vkutils::destroy_image(resources.device, resources.allocator, resources.draw_image);
		vkutils::destroy_image(resources.device, resources.allocator, resources.depth_image);

		resources.draw_image.nullify();
		resources.depth_image.nullify();
	});
}

static auto init_commands(GlobalResources& resources, GlobalDeletionQueue& deletion_queue, PendingFrames& frames) -> void {
	// Create command pools for commands submitted to the graphics queue.
	const VkCommandPoolCreateInfo cmd_pool_create_info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = resources.graphics_queue_family,
	};

	const VkFenceCreateInfo fence_create_info{
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = VK_FENCE_CREATE_SIGNALED_BIT,// Start signaled so we don't stall on the first frame.
	};

	const VkSemaphoreCreateInfo semaphore_create_info{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	};

	for (i32 i = 0; i < NUM_FRAMES_IN_FLIGHT; ++i) {
		VK_VERIFY(vkCreateCommandPool(resources.device, &cmd_pool_create_info, null, &frames[i].command_pool));

		// Allocate the default command buffer that we will be using for rendering.
		const VkCommandBufferAllocateInfo cmd_alloc_info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = frames[i].command_pool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};

		VK_VERIFY(vkAllocateCommandBuffers(resources.device, &cmd_alloc_info, &frames[i].command_buffer));

		// Init sync structures.
		VK_VERIFY(vkCreateFence(resources.device, &fence_create_info, null, &frames[i].render_fence));
		VK_VERIFY(vkCreateSemaphore(resources.device, &semaphore_create_info, null, &frames[i].render_semaphore));
		VK_VERIFY(vkCreateSemaphore(resources.device, &semaphore_create_info, null, &frames[i].swapchain_semaphore));
	}

	// Init immediate command stuff.
	VK_VERIFY(vkCreateCommandPool(resources.device, &cmd_pool_create_info, null, &resources.immediate_submitter.command_pool));

	const VkCommandBufferAllocateInfo immediate_cmd_alloc_info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = resources.immediate_submitter.command_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};

	VK_VERIFY(vkAllocateCommandBuffers(resources.device, &immediate_cmd_alloc_info, &resources.immediate_submitter.cmd));

	// Init immediate sync structures.
	VK_VERIFY(vkCreateFence(resources.device, &fence_create_info, null, &resources.immediate_submitter.fence));

	deletion_queue.enqueue([&resources] {
		ASSERT(resources.immediate_submitter.fence);
		vkDestroyFence(resources.device, resources.immediate_submitter.fence, null);
		resources.immediate_submitter.fence = null;

		ASSERT(resources.immediate_submitter.command_pool);
		vkDestroyCommandPool(resources.device, resources.immediate_submitter.command_pool, null);
		resources.immediate_submitter.command_pool = null;
	});
}

static auto init_descriptors(GlobalResources& resources, GlobalDeletionQueue& deletion_queue, PendingFrames& frames) -> void {
	const vkutils::DescriptorAllocator::PoolSizeRatio sizes[] = {
		vkutils::DescriptorAllocator::PoolSizeRatio{
			.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			.ratio = 1,
		},
	};

	resources.descriptor_allocator.init(resources.device, 10, sizes);

	// Init per-frame descriptor pools.
	for (auto& frame : frames.frames) {
		// Create a descriptor pool.
		static constexpr vkutils::DescriptorAllocator::PoolSizeRatio FRAME_SIZES[] = {
			{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3},
			{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
			{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3},
			{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
		};

		frame.descriptors.init(resources.device, 1000, FRAME_SIZES);
	}

	deletion_queue.enqueue([&resources, &frames] {
		for (i32 i = NUM_FRAMES_IN_FLIGHT - 1; i >= 0; --i) {
			frames[i].descriptors.destroy_pools(resources.device);
		}
	});

	// Init scene data descriptor.
	{
		const VkDescriptorType bindings[] = {
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		};

		// Scene data should be accessible in both vertex and fragment shaders.
		resources.scene_data_descriptor_layout = vkutils::create_descriptor_layout(resources.device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, bindings);

		deletion_queue.enqueue([&resources] {
			vkDestroyDescriptorSetLayout(resources.device, resources.scene_data_descriptor_layout, null);
			resources.scene_data_descriptor_layout = null;
		});
	}

	// Init single image descriptor.
	{
		const VkDescriptorType bindings[] = {
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		};

		// Only accessible in the fragment shader.
		resources.single_image_descriptor_layout = vkutils::create_descriptor_layout(resources.device, VK_SHADER_STAGE_FRAGMENT_BIT, bindings);

		deletion_queue.enqueue([&resources] {
			vkDestroyDescriptorSetLayout(resources.device, resources.single_image_descriptor_layout, null);
			resources.single_image_descriptor_layout = null;
		});
	}
}

static auto init_pipelines(GlobalResources& resources, GlobalDeletionQueue& deletion_queue) -> void {
	// Init mesh pipeline.
	{
		const VkShaderModule vert_shader = vkutils::load_shader_module(SHADER_PATH("colored_triangle_mesh.vert"), resources.device);
		const VkShaderModule frag_shader = vkutils::load_shader_module(SHADER_PATH("tex_image.frag"), resources.device);

		const VkPushConstantRange push_constant{
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
			.offset = 0,
			.size = sizeof(vkutils::GpuDrawPushConstants),
		};

		const VkDescriptorType bindings[] = {
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,// GpuSceneData.
		};

		// @TODO: Actually bind to the actual mesh. Not doing anything right now.
		const auto mesh_descriptor_layout = vkutils::create_descriptor_layout(resources.device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, bindings);

		deletion_queue.enqueue([&resources, mesh_descriptor_layout] {
			vkDestroyDescriptorSetLayout(resources.device, mesh_descriptor_layout, null);
		});

		const VkPipelineLayoutCreateInfo pipeline_layout_create_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &resources.single_image_descriptor_layout,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &push_constant,
		};

		VK_VERIFY(vkCreatePipelineLayout(resources.device, &pipeline_layout_create_info, null, &resources.pipelines.mesh_pipeline_layout));

		resources.pipelines.mesh_pipeline = vkutils::create_graphics_pipeline(resources.device, vkutils::GraphicsPipelineCreateInfo{
			.vert_shader = vert_shader,
			.frag_shader = frag_shader,
			.pipeline_layout = resources.pipelines.mesh_pipeline_layout,
			.color_attachment_format = resources.draw_image.format,
			.depth_attachment_format = resources.depth_image.format,
		});

		// No longer needed.
		vkDestroyShaderModule(resources.device, vert_shader, null);
		vkDestroyShaderModule(resources.device, frag_shader, null);

		// Destroy pipelines.
		deletion_queue.enqueue([&resources] {
			vkDestroyPipeline(resources.device, resources.pipelines.mesh_pipeline, null);
			vkDestroyPipelineLayout(resources.device, resources.pipelines.mesh_pipeline_layout, null);
		});
	}
}

static auto init_defaults(GlobalResources& resources, GlobalDeletionQueue& deletion_queue) -> void {
	const u32 black = glm::packUnorm4x8(glm::vec4{0.f, 0.f, 0.f, 0.f});
	const u32 magenta = glm::packUnorm4x8(glm::vec4{1.f, 0.f, 1.f, 1.f});

	// 16x16 texture.
	static constexpr i32 RESOLUTION = 16;
	u32 checkerboard_pixels[math::square(RESOLUTION)];
	for (i32 x = 0; x < RESOLUTION; ++x) {
		for (i32 y = 0; y < RESOLUTION; ++y) {
			checkerboard_pixels[x + y * RESOLUTION] = ((x % 2) ^ (y % 2)) ? magenta : black;
		}
	}

	resources.error_checkerboard_image = vkutils::cmd_create_image_from_data(checkerboard_pixels, resources.device, resources.graphics_queue, resources.immediate_submitter, resources.allocator, VkExtent2D{RESOLUTION, RESOLUTION}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT, false);

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

	VK_VERIFY(vkCreateSampler(resources.device, &linear_sampler_create_info, null, &resources.sampler_linear));
	VK_VERIFY(vkCreateSampler(resources.device, &nearest_sampler_create_info, null, &resources.sampler_nearest));

	deletion_queue.enqueue([&resources] {
		vkDestroySampler(resources.device, resources.sampler_nearest, null);
		vkDestroySampler(resources.device, resources.sampler_linear, null);

		vkutils::destroy_image(resources.device, resources.allocator, resources.error_checkerboard_image);
	});
}

auto init(
	ExecContext& context,
	Res<GlobalResources> resources,
	Res<GlobalDeletionQueue> deletion_queue,
	Res<PendingFrames> frames,
	Res<const WindowConfig> window_config,
	Res<IsRendering> is_rendering
) -> void {
	static constexpr auto WINDOW_FLAGS = static_cast<SDL_WindowFlags>(SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
	resources->window = SDL_CreateWindow(window_config->title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, window_config->extent.width, window_config->extent.height, WINDOW_FLAGS);

	// Create vkb instance.
	vkb::InstanceBuilder instance_builder;

	const auto instance = instance_builder
		.set_app_name("GanEngine")
		.request_validation_layers(DEBUG_BUILD)// Only enable validation layers in debug build configuration.
		.use_default_debug_messenger()
		.require_api_version(1, 3, 0)
		.build()
		.value();

	resources->instance = instance.instance;
	resources->debug_messenger = instance.debug_messenger;

	ASSERT(resources->instance);
	ASSERT(resources->debug_messenger);

	// Create a surface to render to.
	SDL_Vulkan_CreateSurface(resources->window, resources->instance, &resources->surface);
	ASSERT(resources->surface);

	const VkPhysicalDeviceVulkan13Features features_1_3{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
		.synchronization2 = true,
		.dynamicRendering = true,
	};

	const VkPhysicalDeviceVulkan12Features features_1_2{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.descriptorIndexing = true,
		.bufferDeviceAddress = true,
	};

	// Let vkb select the most appropriate GPU.
	vkb::PhysicalDeviceSelector selector{instance};
	vkb::PhysicalDevice physical_device = selector
		.set_minimum_version(1, 3)
		.set_required_features_13(features_1_3)
		.set_required_features_12(features_1_2)
		.set_surface(resources->surface)
		.select()
		.value();

	vkb::DeviceBuilder device_builder{physical_device};
	vkb::Device device = device_builder.build().value();

	resources->device = device.device;
	resources->physical_device = device.physical_device;

	// Init vma allocator.
	const VmaAllocatorCreateInfo alloc_create_info{
		.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
		.physicalDevice = resources->physical_device,
		.device = resources->device,
		.instance = resources->instance,
	};

	VK_VERIFY(vmaCreateAllocator(&alloc_create_info, &resources->allocator));

	resources->graphics_queue = device.get_queue(vkb::QueueType::graphics).value();
	resources->graphics_queue_family = device.get_queue_index(vkb::QueueType::graphics).value();

	init_swapchain(*resources, *deletion_queue, window_config->extent.width, window_config->extent.height);
	init_commands(*resources, *deletion_queue, *frames);
	init_descriptors(*resources, *deletion_queue, *frames);
	init_pipelines(*resources, *deletion_queue);
	init_defaults(*resources, *deletion_queue);

	WARN("Initted");

	is_rendering->value = true;
}

auto deinit(
	ExecContext& context,
	Res<GlobalResources> resources,
	Res<GlobalDeletionQueue> deletion_queue,
	Res<PendingFrames> frames
) -> void {
	WARN("Deinitting");

	// Wait for the GPU to stop using these resources before freeing them.
	vkDeviceWaitIdle(resources->device);

	// Destroy per-frame data.
	for (auto& frame : frames->frames) {
		// Destroy transient allocations.
		frame.deletion_queue.flush();

		// Destroy command pool.
		vkDestroyCommandPool(resources->device, frame.command_pool, null);

		// Destroy synchronization structures.
		vkDestroyFence(resources->device, frame.render_fence, null);
		vkDestroySemaphore(resources->device, frame.render_semaphore, null);
		vkDestroySemaphore(resources->device, frame.swapchain_semaphore, null);

		frame.command_pool = null;
		frame.render_fence = null;
		frame.render_semaphore = null;
		frame.swapchain_semaphore = null;
	}

	// Flush global deletion-queue.
	deletion_queue->flush();
}
}
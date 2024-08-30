#include "vk_plugin.hpp"

#include <vma/vk_mem_alloc.h>
#include <SDL.h>
#include <SDL_vulkan.h>

#include <VkBootstrap.h>

#include "defines.hpp"
#include "ecs/app.hpp"
#include "gameplay_framework.hpp"
// @TMP: All of this is tmp until I can come up with better abstractions.

// Asserts if the command being executed does not result in VK_SUCCESS.
#define VK_VERIFY(CMD) { \
		const auto vk_result = CMD; \
		ASSERTF(vk_result == VK_SUCCESS, "{} != VK_SUCCESS!", #CMD); \
 }

static_assert(null == VK_NULL_HANDLE);

struct FrameData {
	VkCommandPool command_pool = null;
	VkCommandBuffer command_buffer = null;

	VkSemaphore swapchain_semaphore = null;
	VkSemaphore render_semaphore = null;
	VkFence render_fence = null;
};

// Number of frames in-flight.
static constexpr usize FRAMES_IN_FLIGHT_COUNT = 2;

namespace vkutils {
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

	auto draw() -> void {
		WARN("Drawing {}", current_frame_count);

		auto& current_frame = get_current_frame_data();

		// Wait until the GPU has finished rendering the previous frame before attempting to draw the next frame.
		VK_VERIFY(vkWaitForFences(device, 1, &current_frame.render_fence, true, UINT64_MAX));
		VK_VERIFY(vkResetFences(device, 1, &current_frame.render_fence));// Reset after waiting.

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

		VK_VERIFY(vkBeginCommandBuffer(cmd, &cmd_begin_info));

		// Make the swapchain image writeable before rendering to it.
		vkutils::transition_image_layout(cmd, swapchain_images[swapchain_image_index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

		VkClearColorValue clear_color;
		const auto flash = std::abs(std::sin(static_cast<f32>(current_frame_count) / 120));
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
		vkCmdClearColorImage(cmd, swapchain_images[swapchain_image_index], VK_IMAGE_LAYOUT_GENERAL, &clear_color, 1, &clear_range);

		// Make swapchain presentable.
		vkutils::transition_image_layout(cmd, swapchain_images[swapchain_image_index], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

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

		engine.window = SDL_CreateWindow(window_config.title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, window_config.extent.width, window_config.extent.height, SDL_WINDOW_VULKAN);

		// Create vk instance.
		vkb::InstanceBuilder instance_builder;

		auto instance = instance_builder
			.set_app_name("GanEngine")
			.request_validation_layers(DEBUG_BUILD)// Enable validation layers in debug-builds.
			.use_default_debug_messenger()
			.require_api_version(1, 3, 0)
			.build()
			.value();

		engine.instance = instance.instance;
		engine.debug_messenger = instance.debug_messenger;

		ASSERT(engine.instance);
		ASSERT(engine.debug_messenger);

		// Create surface to render to.
		ASSERT(!engine.surface);
		SDL_Vulkan_CreateSurface(engine.window, engine.instance, &engine.surface);
		ASSERT(engine.surface);

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
			.set_surface(engine.surface)
			.select()
			.value();

		vkb::DeviceBuilder device_builder{physical_device};
		vkb::Device device = device_builder.build().value();

		engine.physical_device = physical_device.physical_device;
		engine.device = device.device;

		// Create swapchain.
		engine.create_swapchain(window_config);

		// Get the graphics queue.
		engine.graphics_queue = device.get_queue(vkb::QueueType::graphics).value();
		engine.graphics_queue_family = device.get_queue_index(vkb::QueueType::graphics).value();

		// Init commands.
		engine.init_commands();

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

	fmt::println("IsRendering == {}", is_rendering->value);

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
			.thread = Thread::ANY,
		})
		.register_system<vk::shutdown>(SystemDesc{
			.group = get_group_id<group::RenderShutdown>(),
			.event = get_event_id<event::OnShutdown>(),
			.priority = Priority::LOW,
			.thread = Thread::MAIN,
		});
}


#if 0
struct VkInitSystem {
	explicit constexpr VkInitSystem(WindowConfig window_config)
		: window_config{std::move(window_config)} {}

	[[nodiscard]] static auto get_access_requirements() -> AccessRequirements {
		return {
			.resources{
				.writes = ResMask::make<res::VkEngine, res::IsRendering>(),
			},
		};
	}

	auto execute(ExecContext& context) -> void {
		// Init SDL.
		SDL_Init(SDL_INIT_VIDEO);

		auto& engine = context.get_mut_res<res::VkEngine>();

		static constexpr auto WINDOW_FLAGS = SDL_WINDOW_VULKAN;
		engine.window = SDL_CreateWindow(window_config.title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, window_config.extent.width, window_config.extent.height, WINDOW_FLAGS);

		// Create VkInstance.
		context.get_mut_res<res::IsRendering>().value = true;// Begin rendering.
	}

	WindowConfig window_config;
};

struct VkDrawSystem {
	[[nodiscard]] static auto get_access_requirements() -> AccessRequirements {
		return {
			.resources{
				.reads = ResMask::make<res::VkEngine>(),
				.writes = ResMask::make<res::RequestExit, res::IsRendering>(),
			},
		};
	}

	auto execute(ExecContext& context) -> void {
		auto& is_rendering = context.get_mut_res<res::IsRendering>().value;

		// Handle SDL events.
		{
			SDL_Event event;
			while (SDL_PollEvent(&event)) {
				switch (event.type) {
				case SDL_QUIT: {
					context.get_mut_res<res::RequestExit>().value = true;
					return;
				}
				case SDL_WINDOWEVENT: {
					switch (event.window.event) {
					case SDL_WINDOWEVENT_MINIMIZED:
						is_rendering = false;
						break;

					case SDL_WINDOWEVENT_RESTORED:
						is_rendering = true;
						break;
					}
				}
				}
			}
		}

		fmt::println("IsRendering == {}", is_rendering);

		if (!is_rendering) {// Window is minimized or something.
			std::this_thread::sleep_for(std::chrono::milliseconds{100});// @TODO: Implement actual frame-pacing.
			return;
		}


	}
};

struct VkShutdownSystem {
	[[nodiscard]] static auto get_access_requirements() -> AccessRequirements {
		return {
			.resources{
				.writes = ResMask::make<res::VkEngine>(),
			},
		};
	}

	auto execute(ExecContext& context) -> void {
		fmt::println("Begin destroy window!");

		auto& engine = context.get_mut_res<res::VkEngine>();

		SDL_Quit();

		SDL_DestroyWindow(engine.window);
		engine.window = null;

		fmt::println("Destroyed window!");
	}
};

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
		.register_system<VkInitSystem>(SystemDesc{
			.group = get_group_id<group::RenderInit>(),
			.event = get_event_id<event::OnInit>(),
			.priority = Priority::HIGH,
			.thread = Thread::MAIN,// SDL must be initialized and destroyed on the same thread.
		}, std::move(window_config))
		.register_system<VkDrawSystem>(SystemDesc{
			.group = get_group_id<group::RenderFrame>(),
			.event = get_event_id<event::OnUpdate>(),
			.priority = Priority::HIGH,
		})
		.register_system<VkShutdownSystem>(SystemDesc{
			.group = get_group_id<group::RenderShutdown>(),
			.event = get_event_id<event::OnShutdown>(),
			.thread = Thread::MAIN,// SDL must be initialized and destroyed on the same thread.
		});
}
#endif
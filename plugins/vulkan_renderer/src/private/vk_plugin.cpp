#include "vk_plugin.hpp"

#include <vma/vk_mem_alloc.h>
#include <SDL.h>
#include <SDL_vulkan.h>

#include "ecs/app.hpp"
#include "gameplay_framework.hpp"

static_assert(null == VK_NULL_HANDLE);

namespace res {
struct VkEngine {
	SDL_Window* window = null;

	VkInstance instance = null;
	VkDebugUtilsMessengerEXT debug_messenger = null;// Debug output handle.
	VkPhysicalDevice physical_device = null;
	VkDevice device = null;
	VkSurfaceKHR surface = null;
};
}

struct VkInitSystem {
	[[nodiscard]] static auto get_access_requirements() -> AccessRequirements {
		return {
			.resources{
				.reads = ResMask::make<res::WindowConfig>(),
				.writes = ResMask::make<res::VkEngine, res::IsRendering>(),
			},
		};
	}

	auto execute(ExecContext& context) -> void {
		// Init SDL.
		SDL_Init(SDL_INIT_VIDEO);

		auto& engine = context.get_mut_res<res::VkEngine>();
		const auto& window_config = context.get_res<res::WindowConfig>();

		static constexpr auto WINDOW_FLAGS = SDL_WINDOW_VULKAN;
		engine.window = SDL_CreateWindow(window_config.title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, window_config.extent.width, window_config.extent.height, WINDOW_FLAGS);

		// Create VkInstance.


		context.get_mut_res<res::IsRendering>().value = true;// Begin rendering.
	}
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
			}
		};
	}

	auto execute(ExecContext& context) -> void {
		WARN("Begin destroy window!");

		auto& engine = context.get_mut_res<res::VkEngine>();

		SDL_DestroyWindow(engine.window);
		engine.window = null;

		WARN("Destroyed window!");
	}
};

auto VkPlugin::init(App& app) -> void {
	app
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
		.register_resource<res::IsRendering>()
		.register_resource(std::move(window_config))
		.register_resource<res::VkEngine>()
		.register_system<VkInitSystem>(SystemDesc{
			.group = get_group_id<group::RenderInit>(),
			.event = get_event_id<event::OnInit>(),
			.priority = Priority::HIGH,
		})
		.register_system<VkDrawSystem>(SystemDesc{
			.group = get_group_id<group::RenderFrame>(),
			.event = get_event_id<event::OnUpdate>(),
			.priority = Priority::HIGH,
		})
		#if 01
		.register_system<VkShutdownSystem>(SystemDesc{
			.group = get_group_id<group::RenderShutdown>(),
			.event = get_event_id<event::OnShutdown>(),
		});
		#endif
}
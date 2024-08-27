#include "vk_plugin.hpp"

#include <vma/vk_mem_alloc.h>
#include <SDL.h>
#include <SDL_vulkan.h>

#include "ecs/app.hpp"

static_assert(null == VK_NULL_HANDLE);

namespace res {
struct VkEngine {
	SDL_Window* window = null;

	VkInstance instance = null;
	VkPhysicalDevice physical_device = null;
	VkDevice device = null;
};
}

struct VkInitSystem {
	[[nodiscard]] static auto get_access_requirements() -> AccessRequirements {
		return {
			.resources{
				.reads = ResMask::make<res::WindowConfig>(),
				.writes = ResMask::make<res::VkEngine>(),
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
		auto& engine = context.get_mut_res<res::VkEngine>();

		SDL_DestroyWindow(engine.window);
		engine.window = null;
	}
};

auto VkPlugin::init(App& app) -> void {
	app
		.register_group<group::RenderInit>(Ordering{
			.within = GroupId::invalid_id(),
		})
		.register_group<group::RenderShutdown>(Ordering{
			.within = GroupId::invalid_id(),
		})
		.register_resource(std::move(window_config))
		.register_resource<res::VkEngine>()
		.register_system<VkInitSystem>(SystemDesc{
			.group = get_group_id<group::RenderInit>(),
			.event = get_event_id<event::OnInit>(),
			.priority = Priority::HIGH,
		})
		#if 01
		.register_system<VkShutdownSystem>(SystemDesc{
			.group = get_group_id<group::RenderShutdown>(),
			.event = get_event_id<event::OnShutdown>(),
		});
		#endif
}
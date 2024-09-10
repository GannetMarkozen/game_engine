#include "vk_plugin.hpp"

#include "vk_engine.hpp"

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

		engine.init(window_config.title, window_config.extent.width, window_config.extent.height);

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

		const auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(current_time_point - previous_time_point).count();

		const auto ms = static_cast<f32>(microseconds) / 1000;
		const auto framerate = static_cast<i32>(std::round(1000000.f / microseconds));

		ImGui::Text("Framerate: %i", framerate);
		ImGui::Text("Ms: %.3f", ms);

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
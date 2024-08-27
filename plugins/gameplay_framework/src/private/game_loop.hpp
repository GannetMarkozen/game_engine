#pragma once

#include "gameplay_framework.hpp"
#include "ecs/world.hpp"
#include "ecs/defaults.hpp"
#include "threading/task.hpp"

namespace gameplay_framework {
struct EndFrameSystem {
	[[nodiscard]] static auto get_access_requirements() -> AccessRequirements {
		return {
			.resources{
				.reads = ResMask::make<res::RequestExit>(),
			},
		};
	}

	auto execute(ExecContext& context) const -> void {
		const auto request_exit = context.get_res<res::RequestExit>().value;
		if (!request_exit) {// Dispatch the OnUpdate event so long as RequestExit is false.
			std::this_thread::sleep_for(std::chrono::milliseconds{10});// @TMP:
			context.world.dispatch_event<event::OnUpdate>();
		} else {// Dispatch OnShutdown event. Final event before the App exits.
			WARN("Dispatching Shutdown!");
			context.world.dispatch_event<event::OnShutdown>();
		}
	}
};

struct BeginUpdateLoopSystem : public EndFrameSystem {
	using EndFrameSystem::EndFrameSystem;
};
}
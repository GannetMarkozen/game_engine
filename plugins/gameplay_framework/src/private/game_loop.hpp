#pragma once

#include "gameplay_framework.hpp"
#include "ecs/world.hpp"
#include "ecs/defaults.hpp"

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
			context.world.dispatch_event<event::OnUpdate>();
		} else {
			// @TODO: Make this part automatic. Shouldn't hang forever. Should count active threads or something.
			context.world.is_pending_destruction = true;
			task::pending_shutdown = true;
		}
	}
};

struct BeginUpdateLoopSystem : public EndFrameSystem {
	using EndFrameSystem::EndFrameSystem;
};
}
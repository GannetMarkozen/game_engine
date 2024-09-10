#pragma once

#include "core_include.hpp"
#include "resources.hpp"

namespace res {
struct RequestExit;
}

namespace renderer {
// Initializes vulkan resources.
auto init(
	ExecContext& context,
	Res<GlobalResources> resources,
	Res<GlobalDeletionQueue> deletion_queue,
	Res<PendingFrames> frames,
	Res<const WindowConfig> window_config,
	Res<IsRendering> is_rendering
) -> void;

auto deinit(
	ExecContext& context,
	Res<GlobalResources> resources,
	Res<GlobalDeletionQueue> deletion_queue,
	Res<PendingFrames> frames
) -> void;
}

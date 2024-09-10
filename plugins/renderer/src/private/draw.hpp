#pragma once

#include "core_include.hpp"
#include "resources.hpp"

#include "gameplay_framework.hpp"

namespace renderer {
auto draw(
	ExecContext& context,
	Res<const GlobalResources> resources,
	Res<PendingFrames> frames,
	Res<res::RequestExit> request_exit,
	Res<IsRendering> is_rendering,
	Query<
		const Transform,
		const Camera
	>& camera_query,
	Query<
		const Transform,
		const SharedPtr<Mesh>
	>& primitive_query,
	u64& current_frame
) -> void;
}
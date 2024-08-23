#pragma once

namespace ecs { struct App; }

struct VkPlugin {
	auto init(ecs::App& app) -> void;
};
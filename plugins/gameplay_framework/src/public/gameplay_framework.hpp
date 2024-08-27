#pragma once

#include "core_include.hpp"

namespace ecs { struct App; }

struct GameplayFrameworkPlugin {
	auto init(ecs::App& app) const -> void;
};
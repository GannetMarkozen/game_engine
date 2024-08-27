#pragma once

#include "core_include.hpp"

struct App;

namespace group {
struct EndGameFrame {};
}

namespace res {
struct RequestExit {
	bool value = false;
};
}

struct GameplayFrameworkPlugin {
	auto init(App& app) const -> void;
};
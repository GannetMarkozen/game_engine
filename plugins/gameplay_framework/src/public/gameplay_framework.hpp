#pragma once

struct App;

namespace group {
struct EndGameFrame {};
struct Movement {};
}

namespace res {
struct RequestExit {
	bool value = false;
};
}

struct GameplayFrameworkPlugin {
	auto init(App& app) const -> void;
};
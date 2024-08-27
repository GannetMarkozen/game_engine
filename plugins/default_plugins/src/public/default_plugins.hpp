#pragma once

struct App;

struct DefaultPlugins {
	auto init(App& app) const -> void;
};
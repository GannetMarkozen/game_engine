#pragma once

#include "gameplay_framework.hpp"
#include "renderer_plugin.hpp"

struct App;

struct DefaultPlugins {
	GameplayFrameworkPlugin gameplay_framework;
	RendererPlugin renderer;

	auto init(App& app) -> void;
};
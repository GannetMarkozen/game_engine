#pragma once

#include "gameplay_framework.hpp"
#include "vk_plugin.hpp"

struct App;

struct DefaultPlugins {
	GameplayFrameworkPlugin gameplay_framework;
	VkPlugin vk;

	auto init(App& app) -> void;
};
#pragma once

struct App;

struct VkPlugin {
	auto init(App& app) -> void;
};
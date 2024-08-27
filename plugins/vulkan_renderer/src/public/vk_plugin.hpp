#pragma once

#define VULKAN_HPP_NO_EXCEPTIONS

#include <vulkan/vulkan.hpp>

#include "types.hpp"

struct App;

namespace res {
struct WindowConfig {
	const char* [[clang::lifetimebound]] title = "GanEngine";
	struct {
		usize width = 1920;
		usize height = 1080;
	} extent;
};

struct IsRendering {
	bool value = false;
};
}

namespace group {
struct RenderInit {};
struct RenderShutdown {};
}

struct VkPlugin {
	res::WindowConfig window_config;
	VkPresentModeKHR present_mode = VK_PRESENT_MODE_MAILBOX_KHR;

	auto init(App& app) -> void;
};
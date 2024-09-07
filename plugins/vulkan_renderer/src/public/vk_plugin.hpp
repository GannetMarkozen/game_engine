#pragma once

#define VULKAN_HPP_NO_EXCEPTIONS

#include <vulkan/vulkan.hpp>

#include "types.hpp"

struct App;

struct WindowConfig {
	const char* [[clang::lifetimebound]] title = "GanEngine";
	struct {
		usize width = 2560;
		usize height = 1440;
	} extent;

	VkPresentModeKHR present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
};

namespace res {
struct IsRendering {
	bool value = false;
};
}

namespace group {
struct RenderInit {};
struct RenderShutdown {};
}

struct VkPlugin {
	WindowConfig window_config;

	auto init(App& app) -> void;
};
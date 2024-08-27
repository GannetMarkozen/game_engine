#pragma once

#define VULKAN_HPP_NO_EXCEPTIONS

#include <vulkan/vulkan.hpp>

#include "types.hpp"

struct App;

struct WindowConfig {
	const char* [[clang::lifetimebound]] title = "GanEngine";
	struct {
		usize width = 1920;
		usize height = 1080;
	} extent;
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
	VkPresentModeKHR present_mode = VK_PRESENT_MODE_MAILBOX_KHR;

	auto init(App& app) -> void;
};
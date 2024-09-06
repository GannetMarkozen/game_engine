#pragma once

// Asserts if the command being executed does not result in VK_SUCCESS. Only checks in Debug builds.
#define VK_VERIFY(CMD) { \
		[[maybe_unused]] const VkResult vk_result = CMD; \
		ASSERTF(vk_result == VK_SUCCESS, "{} != VK_SUCCESS!", #CMD); \
	}
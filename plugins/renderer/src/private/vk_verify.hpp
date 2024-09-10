#pragma once

#include "assert.hpp"

#define VK_VERIFY(CMD) { \
		const VkResult vk_result = CMD; \
		ASSERT(vk_result == VK_SUCCESS); \
	}
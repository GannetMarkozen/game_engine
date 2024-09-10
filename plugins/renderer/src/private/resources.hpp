#pragma once

#include "core_include.hpp"
#include "renderer_plugin.hpp"
#include "vk_utils.hpp"

#include <vulkan/vulkan_core.h>

#include <vma/vk_mem_alloc.h>

struct SDL_Window;

namespace renderer {
struct Pipelines {
	VkPipeline mesh_pipeline = null;
	VkPipelineLayout mesh_pipeline_layout = null;
};

struct GlobalResources {
	VkInstance instance = null;

	VkDevice device = null;
	VkPhysicalDevice physical_device = null;
	VmaAllocator allocator = null;
	VkDebugUtilsMessengerEXT debug_messenger = null;

	VkQueue graphics_queue = null;
	u32 graphics_queue_family = UINT32_MAX;

	vkutils::ImmediateSubmitter immediate_submitter;

	vkutils::DescriptorAllocator descriptor_allocator;

	// @NOTE: Only supports one window for now.
	SDL_Window* window = null;
	VkSurfaceKHR surface = null;
	vkutils::Swapchain swapchain;
	vkutils::Image draw_image;
	vkutils::Image depth_image;

	// GpuSceneData descriptor layout.
	VkDescriptorSetLayout scene_data_descriptor_layout = null;

	// Mesh draws with a single image push constant.
	VkDescriptorSetLayout single_image_descriptor_layout = null;

	VkSampler sampler_linear = null;
	VkSampler sampler_nearest = null;

	vkutils::Image error_checkerboard_image;

	Pipelines pipelines;
};

struct DeletionQueue {
	constexpr auto enqueue(Fn<void()>&& dtor) -> void {
		dtors.push_back(std::move(dtor));
	}

	auto flush() -> void {
		for (auto it = dtors.rbegin(); it != dtors.rend(); ++it) {
			(*it)();
		}
		dtors.clear();
	}

	Queue<Fn<void()>> dtors;
};

struct GlobalDeletionQueue : public DeletionQueue {
	using DeletionQueue::DeletionQueue;
};

struct Frame {
	VkCommandPool command_pool = null;
	VkCommandBuffer command_buffer = null;
	VkSemaphore swapchain_semaphore = null;
	VkSemaphore render_semaphore = null;
	VkFence render_fence = null;

	DeletionQueue deletion_queue;// Transient deletion-queue (flushed each frame interval).
	vkutils::DescriptorAllocator descriptors;// Transient descriptors (deleted each frame interval).
};

struct PendingFrames {
	template <typename Self>
	[[nodiscard]] constexpr auto operator[](this Self&& self, const std::integral auto index) -> decltype(auto) {
		return (std::forward_like<Self>(self.frames[index]));
	}

	Frame frames[renderer::NUM_FRAMES_IN_FLIGHT];
	u64 current_frame = 0;
};
}
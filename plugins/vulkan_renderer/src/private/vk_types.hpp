#pragma once

#include <vulkan/vulkan_core.h>
#include <vma/vk_mem_alloc.h>

#include <glm/glm.hpp>

#include "types.hpp"

struct AllocatedBuffer {
	VkBuffer buffer;
	VmaAllocation allocation;
	VmaAllocationInfo allocation_info;
};

struct Vertex {
	glm::vec3 position;
	f32 uv_x;
	glm::vec3 normal;
	f32 uv_y;
	glm::vec4 color;
};

// Holds the resources needed for a mesh.
struct GpuMeshBuffers {
	AllocatedBuffer index_buffer;
	AllocatedBuffer vertex_buffer;
	VkDeviceAddress vertex_buffer_address;
};

// Push constants for mesh objects to draw.
struct GpuDrawPushConstants {
	glm::mat4 render_matrix;
	VkDeviceAddress vertex_buffer;
};
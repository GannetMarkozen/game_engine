#pragma once

#include <vulkan/vulkan_core.h>
#include <vma/vk_mem_alloc.h>

#include <glm/glm.hpp>

#include "types.hpp"

#include "vk_verify.hpp"

namespace res {
struct VkEngine;
}

struct AllocatedBuffer {
	VkBuffer buffer;
	VmaAllocation allocation;
	VmaAllocationInfo allocation_info;
};

struct AllocatedImage {
	VkImage image = null;
	VkImageView image_view = null;
	VmaAllocation allocation = null;
	VkExtent3D extent = {};
	VkFormat format;
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

struct GpuSceneData {
	glm::mat4 view;
	glm::mat4 proj;
	glm::mat4 view_proj;
	glm::vec4 ambient_color;
	glm::vec4 sunlight_direction;// W for intensity.
	glm::vec4 sunlight_color;
};

// Allocator for descriptor sets. Contains multiple descriptor set pools that are re-used.
struct DescriptorAllocatorGrowable {
	struct PoolSizeRatio {
		VkDescriptorType type;
		f32 ratio;
	};

	auto init(VkDevice device, const u32 initial_sets, const Span<const PoolSizeRatio> pool_ratios) -> void {
		ratios.clear();
		ratios.append_range(pool_ratios);

		auto new_pool = create_pool(device, initial_sets, pool_ratios);

		// Grow it next allocation.
		sets_per_pool = std::min<u32>(initial_sets * 1.5, 4096);

		ready_pools.push_back(new_pool);
	}

	auto clear_pools(VkDevice device) -> void {
		ready_pools.reserve(ready_pools.size() + full_pools.size());

		for (const auto pool : ready_pools) {
			vkResetDescriptorPool(device, pool, 0);
		}

		for (const auto pool : full_pools) {
			vkResetDescriptorPool(device, pool, 0);

			ready_pools.push_back(pool);
		}

		full_pools.clear();
	}

	auto destroy_pools(VkDevice device) -> void {
		for (const auto pool : ready_pools) {
			vkDestroyDescriptorPool(device, pool, null);
		}
		for (const auto pool : full_pools) {
			vkDestroyDescriptorPool(device, pool, null);
		}
		ready_pools.clear();
		full_pools.clear();
	}

	[[nodiscard]] auto allocate(VkDevice device, const VkDescriptorSetLayout& layout, void* next = null) -> VkDescriptorSet {
		ASSERT(layout);

		auto pool_to_use = find_or_create_pool(device);
		ASSERT(pool_to_use);


		VkDescriptorSetAllocateInfo alloc_info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.pNext = next,
			.descriptorPool = pool_to_use,
			.descriptorSetCount = 1,
			.pSetLayouts = &layout,
		};

		VkDescriptorSet descriptor_set;
		const auto result = vkAllocateDescriptorSets(device, &alloc_info, &descriptor_set);

		// If the allocation fails due to the pool being full, allocate a find or allocate a new pool and use that one instead.
		// Move this pool into the full_pools array.
		if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL) {
			full_pools.push_back(pool_to_use);

			pool_to_use = find_or_create_pool(device);
			ASSERT(pool_to_use);

			alloc_info.descriptorPool = pool_to_use;

			VK_VERIFY(vkAllocateDescriptorSets(device, &alloc_info, &descriptor_set));
		} else {
			ASSERT(result == VK_SUCCESS);
		}

		// @NOTE: This is weirdly done where find_or_create_pool will pop_back a pool just for it to be re-pushed. Sort of unnecessary.
		ready_pools.push_back(pool_to_use);

		return descriptor_set;
	}

private:
	[[nodiscard]] auto find_or_create_pool(VkDevice device) -> VkDescriptorPool {
		VkDescriptorPool new_pool;
		if (ready_pools.empty()) {
			// Need to create a new pool.
			new_pool = create_pool(device, sets_per_pool, ratios);

			sets_per_pool = std::min<u32>(sets_per_pool * 1.5, 4092);
		} else {
			new_pool = std::move(ready_pools.back());
			ready_pools.pop_back();
		}

		return new_pool;
	}

	[[nodiscard]] auto create_pool(VkDevice device, const u32 set_count, const Span<const PoolSizeRatio> pool_ratios) -> VkDescriptorPool {
		Array<VkDescriptorPoolSize> pool_sizes;
		pool_sizes.reserve(pool_ratios.size());
		for (const auto& ratio : pool_ratios) {
			pool_sizes.push_back(VkDescriptorPoolSize{
				.type = ratio.type,
				.descriptorCount = static_cast<u32>(ratio.ratio * set_count),
			});
		}

		const VkDescriptorPoolCreateInfo desc_pool_create_info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.flags = 0,
			.maxSets = set_count,
			.poolSizeCount = static_cast<u32>(pool_sizes.size()),
			.pPoolSizes = pool_sizes.data(),
		};

		VkDescriptorPool new_pool;
		VK_VERIFY(vkCreateDescriptorPool(device, &desc_pool_create_info, null, &new_pool));

		return new_pool;
	}

	Array<PoolSizeRatio> ratios;
	Array<VkDescriptorPool> full_pools;
	Array<VkDescriptorPool> ready_pools;
	u32 sets_per_pool;
};

struct DescriptorWriter {
	auto write_image(const u32 binding, VkImageView image_view, VkSampler sampler, const VkImageLayout layout, const VkDescriptorType type) -> DescriptorWriter& {
		auto& info = image_infos.emplace_back(VkDescriptorImageInfo{
			.sampler = sampler,
			.imageView = image_view,
			.imageLayout = layout,
		});

		writes.push_back(VkWriteDescriptorSet{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = null,// Left empty for now until we need to write it.
			.descriptorCount = 1,
			.descriptorType = type,
			.pImageInfo = &info,
		});

		return *this;
	}

	auto write_buffer(const u32 binding, VkBuffer buffer, const usize size, const usize offset, const VkDescriptorType type) -> DescriptorWriter& {
		auto& info = buffer_infos.emplace_back(VkDescriptorBufferInfo{
			.buffer = buffer,
			.offset = offset,
			.range = size,
		});

		writes.push_back(VkWriteDescriptorSet{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = null,// Left empty for now until we need to write it.
			.dstBinding = binding,
			.descriptorCount = 1,
			.descriptorType = type,
			.pBufferInfo = &info,
		});

		return *this;
	}

	auto clear() -> void {
		image_infos.clear();
		buffer_infos.clear();
		writes.clear();
	}

	auto update_set(VkDevice device, const VkDescriptorSet set) -> void {
		for (auto& write : writes) {
			write.dstSet = set;
		}

		vkUpdateDescriptorSets(device, static_cast<u32>(writes.size()), writes.data(), 0, null);
	}

	Queue<VkDescriptorImageInfo> image_infos;
	Queue<VkDescriptorBufferInfo> buffer_infos;
	Array<VkWriteDescriptorSet> writes;
};

struct MaterialPipeline {
	VkPipeline pipeline;
	VkPipelineLayout layout;
};

enum class MaterialPass : u8 {
	OPAQUE, TRANSLUCENT,
};

struct MaterialInstance {
	MaterialPipeline* pipeline;
	VkDescriptorSet descriptor;
	MaterialPass pass;
};

struct RenderObject {
	MaterialInstance* material;
	glm::mat4 transform;
	u32 index_count, first_index;
	VkBuffer index_buffer;
	VkDeviceAddress vertex_buffer_address;
};

struct MaterialProperties {
	
};

struct GltfMetallicRoughness {
	struct MaterialConstants {
		glm::vec4 color_factors;
		glm::vec4 metal_roughness_factors;

		// Padding. Uniform buffers have an alignment of 256 bytes this extra data is required anyways.
		glm::vec4 extra[14];
	};

	static_assert(sizeof(MaterialConstants) == 256);

	struct MaterialResources {
		AllocatedImage color_image;
		VkSampler color_sampler;
		AllocatedImage metal_roughness_image;
		VkSampler metal_roughness_image_sampler;
		VkBuffer data_buffer;
		u32 data_buffer_offset;
	};

	auto build_pipelines(res::VkEngine& engine) -> void;
	auto clear_resources(VkDevice device) -> void;
	auto write_materials(VkDevice device, const MaterialPass pass, const MaterialResources& resources, DescriptorAllocatorGrowable& desc_allocator) -> MaterialInstance;

	MaterialPipeline opaque_pipeline;
	MaterialPipeline translucent_pipeline;
	VkDescriptorSetLayout material_layout;// Shared between opaque and translucent pipelines.
	DescriptorWriter writer;
};

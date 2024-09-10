#pragma once

#include "core_include.hpp"
#include "vk_verify.hpp"
#include "renderer_types.hpp"

#include <cstdint>
#include <vulkan/vulkan_core.h>

#include <vma/vk_mem_alloc.h>

#include <VkBootstrap.h>

struct Mesh;

namespace vkutils {
struct Swapchain {
	VkSwapchainKHR swapchain = null;
	VkExtent2D extent;
	Array<VkImage> images;
	Array<VkImageView> image_views;
	VkFormat format;
};

struct Image {
	auto nullify() -> void {
		image = null;
		image_view = null;
		allocation = null;
		allocation_info = {};
		extent = {};
		format = {};
	}

	VkImage image = null;
	VkImageView image_view = null;
	VmaAllocation allocation = null;
	VmaAllocationInfo allocation_info;
	VkExtent3D extent;
	VkFormat format;
};

struct Buffer {
	auto nullify() -> void {
		buffer = null;
		allocation = null;
		allocation_info = {};
	}

	VkBuffer buffer = null;
	VmaAllocation allocation = null;
	VmaAllocationInfo allocation_info;
};

struct GpuMeshBuffer {
	Buffer vertex_buffer;
	Buffer index_buffer;
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

struct ImmediateSubmitter {
	auto immediate_submit(const VkDevice device, const VkQueue queue, const FnRef<void(VkCommandBuffer cmd)> fn) const -> void {
		ASSERT(device);
		ASSERT(queue);
		ASSERT(cmd);
		ASSERT(command_pool);
		ASSERT(fence);

		VK_VERIFY(vkResetFences(device, 1, &fence));
		VK_VERIFY(vkResetCommandBuffer(cmd, 0));

		const VkCommandBufferBeginInfo cmd_begin_info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};

		VK_VERIFY(vkBeginCommandBuffer(cmd, &cmd_begin_info));

		fn(cmd);

		VK_VERIFY(vkEndCommandBuffer(cmd));

		const VkCommandBufferSubmitInfo cmd_submit_info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
			.commandBuffer = cmd,
		};

		const VkSubmitInfo2 submit_info{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
			.commandBufferInfoCount = 1,
			.pCommandBufferInfos = &cmd_submit_info,
		};

		// Submit command buffer to the queue and execute it. Block CPU until graphics command has finished execution.
		// @NOTE: Suboptimal.
		VK_VERIFY(vkQueueSubmit2(queue, 1, &submit_info, fence));

		VK_VERIFY(vkWaitForFences(device, 1, &fence, true, UINT64_MAX));
	}

	VkCommandBuffer cmd = null;
	VkCommandPool command_pool = null;
	VkFence fence = null;
};

struct SurfaceRange {
	u32 start_index;
	u32 count;
};

// Allocator for descriptor sets. Contains multiple descriptor set pools that are re-used.
struct DescriptorAllocator {
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

	[[nodiscard]] auto allocate(const VkDevice device, const VkDescriptorSetLayout layout, void* next = null) -> VkDescriptorSet {
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

enum class ColorBlendType : u8 {
	NONE, ALPHA, ADDITIVE,
};

// A simplified create info struct with good defaults.
struct GraphicsPipelineCreateInfo {
	VkShaderModule vert_shader = null;
	VkShaderModule frag_shader = null;
	VkPipelineLayout pipeline_layout = null;
	VkCullModeFlags cull_mode = VK_CULL_MODE_BACK_BIT;
	VkFrontFace front_face = VK_FRONT_FACE_CLOCKWISE;
	VkSampleCountFlagBits msaa_count = VK_SAMPLE_COUNT_1_BIT;
	Optional<VkCompareOp> depth_test = VK_COMPARE_OP_GREATER_OR_EQUAL;
	ColorBlendType color_blend_type = ColorBlendType::NONE;
	VkPrimitiveTopology input_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPolygonMode polygon_mode = VK_POLYGON_MODE_FILL;
	VkFormat color_attachment_format;
	VkFormat depth_attachment_format;
};

[[nodiscard]] inline auto create_graphics_pipeline(const VkDevice device, const GraphicsPipelineCreateInfo& create_info) -> VkPipeline {
	ASSERT(device);
	ASSERT(create_info.pipeline_layout);

	// For dynamic rendering.
	const VkPipelineRenderingCreateInfo rendering_create_info{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = 1,
		.pColorAttachmentFormats = &create_info.color_attachment_format,
		.depthAttachmentFormat = create_info.depth_attachment_format,
	};

	// Make viewport state out of stored viewport and scissor.
	// @NOTE: No support for multiple viewports or scissors.
	const VkPipelineViewportStateCreateInfo viewport_state_create_info{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.pViewports = null,// TODO
		.scissorCount = 1,
		.pScissors = null,// TODO
	};

	static constexpr auto COLOR_WRITE_MASK = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	const auto color_blend_attachment = [&] {
		switch (create_info.color_blend_type) {
		case ColorBlendType::NONE:
			return VkPipelineColorBlendAttachmentState{
				.blendEnable = false,
				.colorWriteMask = COLOR_WRITE_MASK,
			};

		case ColorBlendType::ALPHA:
			return VkPipelineColorBlendAttachmentState{
				.blendEnable = true,
				.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
				.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
				.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
				.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
				.alphaBlendOp =  VK_BLEND_OP_ADD,
				.colorWriteMask = COLOR_WRITE_MASK,
			};

		case ColorBlendType::ADDITIVE:
			return VkPipelineColorBlendAttachmentState{
				.blendEnable = true,
				.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
				.dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
				.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
				.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
				.alphaBlendOp = VK_BLEND_OP_ADD,
				.colorWriteMask = COLOR_WRITE_MASK,
			};
		}
	}();

	ASSERTF(create_info.vert_shader, "Missing vertex shader!");
	ASSERTF(create_info.frag_shader, "Missing fragment shader!");

	// Forces two shader stages for now (vertex and fragment) and executes on "main".
	const VkPipelineShaderStageCreateInfo shader_stage_create_infos[] = {
		VkPipelineShaderStageCreateInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = create_info.vert_shader,
			.pName = "main",
		},
		VkPipelineShaderStageCreateInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = create_info.frag_shader,
			.pName = "main",
		},
	};

	const VkPipelineColorBlendStateCreateInfo color_blending_create_info{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.logicOpEnable = false,
		.logicOp = VK_LOGIC_OP_COPY,
		.attachmentCount = 1,
		.pAttachments = &color_blend_attachment,
	};

	static constexpr VkDynamicState DYNAMIC_STATES[] = {
		VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
	};

	const VkPipelineDynamicStateCreateInfo dynamic_state_create_info{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = std::size(DYNAMIC_STATES),
		.pDynamicStates = DYNAMIC_STATES,
	};

	// Empty. Vertex buffers are bound through uniform buffers and accessed via device address atm.
	const VkPipelineVertexInputStateCreateInfo vertex_input_create_info{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};

	const VkPipelineInputAssemblyStateCreateInfo input_assembly_create_info{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = create_info.input_topology,
		.primitiveRestartEnable = false,
	};

	const VkPipelineRasterizationStateCreateInfo rasterization_create_info{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.rasterizerDiscardEnable = false,
		.polygonMode = create_info.polygon_mode,
		.cullMode = create_info.cull_mode,
		.frontFace = create_info.front_face,
		.lineWidth = 1.f,
	};

	const VkPipelineMultisampleStateCreateInfo msaa_create_info{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = create_info.msaa_count,
		.sampleShadingEnable = create_info.msaa_count != VK_SAMPLE_COUNT_1_BIT,
		.minSampleShading = 1.f,
		.pSampleMask = null,
		.alphaToCoverageEnable = false,
		.alphaToOneEnable = false,
	};

	const VkPipelineDepthStencilStateCreateInfo depth_stencil_create_info{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = create_info.depth_test.has_value(),
		.depthWriteEnable = create_info.depth_test.has_value(),
		.depthCompareOp = *create_info.depth_test,
		.front = {},
		.back = {},
		.minDepthBounds = 0.f,
		.maxDepthBounds = 1.f,
	};

	const VkGraphicsPipelineCreateInfo graphics_pipeline_create_info{
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.pNext = &rendering_create_info,// Dynamic rendering extension.
		.stageCount = std::size(shader_stage_create_infos),// Vertex and fragment shaders (for now).
		.pStages = shader_stage_create_infos,
		.pVertexInputState = &vertex_input_create_info,
		.pInputAssemblyState = &input_assembly_create_info,
		.pViewportState = &viewport_state_create_info,
		.pRasterizationState = &rasterization_create_info,
		.pMultisampleState = &msaa_create_info,
		.pDepthStencilState = &depth_stencil_create_info,
		.pColorBlendState = &color_blending_create_info,
		.pDynamicState = &dynamic_state_create_info,
		.layout = create_info.pipeline_layout,
	};

	VkPipeline pipeline;
	VK_VERIFY(vkCreateGraphicsPipelines(device, null, 1, &graphics_pipeline_create_info, null, &pipeline));

	return pipeline;
}

inline auto transition_image_layout(const VkCommandBuffer cmd, const VkImage image, const VkImageLayout current_layout, const VkImageLayout new_layout) -> void {
	// Special-case for transitioning to depth texture.
	const VkImageAspectFlags aspect_flags = new_layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

	const VkImageMemoryBarrier2 image_memory_barrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,// @NOTE: Suboptimal.
		.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,

		.oldLayout = current_layout,
		.newLayout = new_layout,

		.image = image,

		.subresourceRange{
			.aspectMask = aspect_flags,
			.baseMipLevel = 0,
			.levelCount = VK_REMAINING_MIP_LEVELS,// Hard-coded to affect all mips for now.
			.baseArrayLayer = 0,
			.layerCount = VK_REMAINING_ARRAY_LAYERS,
		},
	};

	const VkDependencyInfo dependency_info{
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &image_memory_barrier,
	};

	vkCmdPipelineBarrier2(cmd, &dependency_info);
}

inline auto copy_image_to_image(const VkCommandBuffer cmd, const VkImage dst, const VkImage src, const VkExtent2D& dst_extent, const VkExtent2D& src_extent) -> void {
	ASSERT(cmd);
	ASSERT(dst);
	ASSERT(src);

	// Utilizes blit.
	// @NOTE: May be more optimal to use a different copy operation if the image extents match.
	const VkImageBlit2 blit_region{
		.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
		.srcSubresource{
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.mipLevel = 0,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
		.srcOffsets{
			VkOffset3D{
				0, 0, 0,
			},
			VkOffset3D{
				.x = static_cast<i32>(src_extent.width),
				.y = static_cast<i32>(src_extent.height),
				.z = 1,
			},
		},
		.dstSubresource{
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.mipLevel = 0,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
		.dstOffsets{
			VkOffset3D{
				0, 0, 0,
			},
			VkOffset3D{
				.x = static_cast<i32>(dst_extent.width),
				.y = static_cast<i32>(dst_extent.height),
				.z = 1,
			},
		},
	};

	const VkBlitImageInfo2 blit_info{
		.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
		.srcImage = src,
		.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.dstImage = dst,
		.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.regionCount = 1,
		.pRegions = &blit_region,
		.filter = VK_FILTER_LINEAR,
	};

	vkCmdBlitImage2(cmd, &blit_info);
}

[[nodiscard]] auto load_shader_module(const char* file_path, const VkDevice device) -> VkShaderModule;

[[nodiscard]] inline auto create_descriptor_layout(const VkDevice device, const VkShaderStageFlags shader_stages, const Span<const VkDescriptorType> binding_types, const VkDescriptorSetLayoutCreateFlags create_flags = 0, void* next = null) -> VkDescriptorSetLayout {
	ASSERT(device);
	ASSERT(shader_stages != 0);
	ASSERT(!binding_types.empty());

	Array<VkDescriptorSetLayoutBinding> bindings;
	bindings.reserve(binding_types.size());
	for (u32 i = 0; i < binding_types.size(); ++i) {
		bindings.push_back(VkDescriptorSetLayoutBinding{
			.binding = i,
			.descriptorType = binding_types[i],
			.descriptorCount = 1,
			.stageFlags = shader_stages,
		});
	}

	ASSERT(bindings.size() == binding_types.size());

	const VkDescriptorSetLayoutCreateInfo create_info{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.pNext = next,
		.flags = create_flags,
		.bindingCount = static_cast<u32>(bindings.size()),
		.pBindings = bindings.data(),
	};

	VkDescriptorSetLayout layout;
	VK_VERIFY(vkCreateDescriptorSetLayout(device, &create_info, null, &layout));

	return layout;
}

[[nodiscard]] inline auto create_swapchain(const VkDevice device, const VkPhysicalDevice physical_device, const VkSurfaceKHR surface, const u32 width, const u32 height,
	const VkPresentModeKHR present_mode = VK_PRESENT_MODE_MAILBOX_KHR, const VkFormat format = VK_FORMAT_B8G8R8A8_UNORM, const VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
) -> Swapchain {
	ASSERT(device);
	ASSERT(physical_device);
	ASSERT(surface);

	vkb::SwapchainBuilder builder{physical_device, device, surface};
	vkb::Swapchain swapchain = builder
		.set_desired_format(VkSurfaceFormatKHR{
			.format = format,
			.colorSpace = color_space,
		})
		.set_desired_present_mode(present_mode)
		.set_desired_extent(width, height)
		.add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.build()
		.value();

	return Swapchain{
		.swapchain = swapchain.swapchain,
		.extent{width, height},
		.images = std::move(swapchain.get_images().value()),
		.image_views = std::move(swapchain.get_image_views().value()),
	};
}

inline auto destroy_swapchain(const VkDevice device, const Swapchain& swapchain) -> void {
	ASSERT(device);
	ASSERT(swapchain.swapchain);

	for (const auto image_view : swapchain.image_views) {
		vkDestroyImageView(device, image_view, null);
	}

	vkDestroySwapchainKHR(device, swapchain.swapchain, null);
}

[[nodiscard]] inline auto create_buffer(const VmaAllocator allocator, const usize size, const VkBufferUsageFlags usage, const VmaMemoryUsage memory_usage) -> Buffer {
	ASSERT(allocator);
	ASSERT(size > 0);

	const VkBufferCreateInfo buffer_create_info{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage,
	};

	const VmaAllocationCreateInfo alloc_create_info{
		.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
		.usage = memory_usage,
	};

	Buffer buffer;
	VK_VERIFY(vmaCreateBuffer(allocator, &buffer_create_info, &alloc_create_info, &buffer.buffer, &buffer.allocation, &buffer.allocation_info));

	return buffer;
}

inline auto destroy_buffer(const VmaAllocator allocator, const Buffer& buffer) -> void {
	ASSERT(allocator);
	vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
}

[[nodiscard]] inline auto create_image(const VkDevice device, const VmaAllocator allocator, const VkExtent2D& extent, const VkFormat format, const VkImageUsageFlags usage, const bool mipmapped = false) -> Image {
	ASSERT(device);
	ASSERT(allocator);

	Image new_image{
		.extent{extent.width, extent.height, 1},
		.format = format,
	};

	const u32 num_mips = mipmapped ? static_cast<u32>(std::floor(std::log2(std::max(extent.width, extent.height)))) + 1 : 1;

	const VkImageCreateInfo image_create_info{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,// Hard-coded for now.
		.format = format,
		.extent{extent.width, extent.height, 1},
		.mipLevels = num_mips,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,// MSAA.
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage,
	};

	const VmaAllocationCreateInfo alloc_create_info{
		.usage = VMA_MEMORY_USAGE_GPU_ONLY,
		.requiredFlags = static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
	};

	VK_VERIFY(vmaCreateImage(allocator, &image_create_info, &alloc_create_info, &new_image.image, &new_image.allocation, &new_image.allocation_info));

	// If it's a depth format need to make sure it has the correct aspect flags.
	const VkImageAspectFlags aspect_flags = format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

	const VkImageViewCreateInfo image_view_create_info{
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = new_image.image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = format,
		.subresourceRange{
			.aspectMask = aspect_flags,
			.baseMipLevel = 0,
			.levelCount = num_mips,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
	};

	VK_VERIFY(vkCreateImageView(device, &image_view_create_info, null, &new_image.image_view));

	return new_image;
}

inline auto destroy_image(const VkDevice device, const VmaAllocator allocator, const Image& image) -> void {
	ASSERT(device);
	ASSERT(allocator);

	vkDestroyImageView(device, image.image_view, null);
	vmaDestroyImage(allocator, image.image, image.allocation);
}

[[nodiscard]] inline auto cmd_create_image_from_data(void* src, const VkDevice device, const VkQueue graphics_queue, const ImmediateSubmitter& immediate_submitter, const VmaAllocator allocator,
	const VkExtent2D& extent, const VkFormat format, const VkImageUsageFlags usage, const bool mipmapped = false
) -> Image {
	ASSERT(src);
	ASSERT(graphics_queue);

	Image new_image = create_image(device, allocator, extent, format, usage, mipmapped);

	const usize buffer_size = extent.width * extent.height * 4;// @NOTE: Assumes RGBA 8 format.
	const Buffer staging_buffer = create_buffer(allocator, buffer_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	void* dst;
	VK_VERIFY(vmaMapMemory(allocator, staging_buffer.allocation, &dst));

	memcpy(dst, src, buffer_size);

	vmaUnmapMemory(allocator, staging_buffer.allocation);

	immediate_submitter.immediate_submit(device, graphics_queue, [&](const VkCommandBuffer cmd) {
		// Transition the new image's layout to a transfer destination.
		vkutils::transition_image_layout(cmd, new_image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		const VkBufferImageCopy copy_region{
			.bufferOffset = 0,
			.bufferRowLength = 0,
			.bufferImageHeight = 0,

			.imageSubresource{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = 0,
				.baseArrayLayer = 0,
				.layerCount = 1,
			},

			.imageExtent{extent.width, extent.height},
		};

		vkCmdCopyBufferToImage(cmd, staging_buffer.buffer, new_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);

		// Now transition the image layout to be optimally read by shaders.
		vkutils::transition_image_layout(cmd, new_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	});

	// No longer need the staging buffer.
	destroy_buffer(allocator, staging_buffer);

	return new_image;
}

[[nodiscard]] inline auto upload_mesh(const VkDevice device, const VmaAllocator allocator, const VkQueue queue, const ImmediateSubmitter& immediate_submitter, const Span<const Vertex> vertices, const Span<const u32> indices) -> GpuMeshBuffer {
	const usize vertex_buffer_size = vertices.size() * sizeof(Vertex);
	const usize index_buffer_size = indices.size() * sizeof(u32);

	// Create vertex buffer.
	Buffer vertex_buffer = vkutils::create_buffer(allocator, vertex_buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	Buffer index_buffer = vkutils::create_buffer(allocator, index_buffer_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

	const VkBufferDeviceAddressInfo device_address_info{
		.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		.buffer = vertex_buffer.buffer,
	};

	VkDeviceAddress vertex_buffer_address = vkGetBufferDeviceAddress(device, &device_address_info);

	const Buffer staging_buffer = vkutils::create_buffer(allocator, vertex_buffer_size + index_buffer_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	void* dst;
	VK_VERIFY(vmaMapMemory(allocator, staging_buffer.allocation, &dst));

	// Copy vertex buffer.
	memcpy(dst, vertices.data(), vertex_buffer_size);

	// Copy index buffer.
	memcpy(reinterpret_cast<char*>(dst) + vertex_buffer_size, indices.data(), index_buffer_size);

	vmaUnmapMemory(allocator, staging_buffer.allocation);

	immediate_submitter.immediate_submit(device, queue, [&](const VkCommandBuffer cmd) {
		const VkBufferCopy vertices_copy{
			.srcOffset = 0,
			.dstOffset = 0,
			.size = vertex_buffer_size,
		};

		const VkBufferCopy indices_copy{
			.srcOffset = vertex_buffer_size,
			.dstOffset = 0,
			.size = index_buffer_size,
		};

		vkCmdCopyBuffer(cmd, staging_buffer.buffer, vertex_buffer.buffer, 1, &vertices_copy);
		vkCmdCopyBuffer(cmd, staging_buffer.buffer, index_buffer.buffer, 1, &indices_copy);
	});

	// Staging buffer is no longer needed after copies.
	destroy_buffer(allocator, staging_buffer);

	return GpuMeshBuffer{
		.vertex_buffer = std::move(vertex_buffer),
		.index_buffer = std::move(index_buffer),
		.vertex_buffer_address = std::move(vertex_buffer_address),
	};
}

[[nodiscard]] auto load_gltf_meshes(const std::filesystem::path& file_path, const VkDevice device, const VmaAllocator allocator, const VkQueue queue, const ImmediateSubmitter& immediate_submitter) -> Optional<Array<SharedPtr<Mesh>>>;
}

struct Mesh {
	String name;
	Array<vkutils::SurfaceRange> surfaces;
	vkutils::GpuMeshBuffer buffer;
};
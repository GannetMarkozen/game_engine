#pragma once

#include <vulkan/vulkan.hpp>

#include "core_include.hpp"
#include "vulkan/vulkan_core.h"

namespace vkutils {
struct PipelineBuilder {
	PipelineBuilder() {
		clear();
	}

	auto clear() -> void {
		shader_stages.clear();

		input_assembly_create_info = VkPipelineInputAssemblyStateCreateInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
		rasterization_create_info = VkPipelineRasterizationStateCreateInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
		color_blend_attachment = VkPipelineColorBlendAttachmentState{};
		multi_sampling_create_info = VkPipelineMultisampleStateCreateInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
		pipeline_layout = {};
		depth_stencil_create_info = VkPipelineDepthStencilStateCreateInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
		rendering_create_info = VkPipelineRenderingCreateInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	}

	auto set_shaders(VkShaderModule vertex_shader, VkShaderModule fragment_shader) -> PipelineBuilder& {
		shader_stages.clear();

		shader_stages.push_back(VkPipelineShaderStageCreateInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vertex_shader,
			.pName = "main",
		});

		shader_stages.push_back(VkPipelineShaderStageCreateInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = fragment_shader,
			.pName = "main",
		});

		return *this;
	}

	auto set_input_topology(const VkPrimitiveTopology topology) -> PipelineBuilder& {
		input_assembly_create_info.topology = topology;
		input_assembly_create_info.primitiveRestartEnable = VK_FALSE;
		return *this;
	}

	auto set_polygon_mode(const VkPolygonMode mode) -> PipelineBuilder& {
		rasterization_create_info.polygonMode = mode;
		rasterization_create_info.lineWidth = 1.f;
		return *this;
	}

	auto set_cull_mode(const VkCullModeFlags cull_mode_flags, const VkFrontFace front_face) -> PipelineBuilder& {
		rasterization_create_info.cullMode = cull_mode_flags;
		rasterization_create_info.frontFace = front_face;
		return *this;
	}

	auto disable_msaa() -> PipelineBuilder& {
		multi_sampling_create_info.sampleShadingEnable = VK_FALSE;
		multi_sampling_create_info.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		multi_sampling_create_info.minSampleShading = 1.f;
		multi_sampling_create_info.pSampleMask = null;
		multi_sampling_create_info.alphaToCoverageEnable = VK_FALSE;
		multi_sampling_create_info.alphaToOneEnable = VK_FALSE;
		return *this;
	}

	auto enable_msaa() -> PipelineBuilder& {
		multi_sampling_create_info.sampleShadingEnable = VK_TRUE;
		multi_sampling_create_info.rasterizationSamples = VK_SAMPLE_COUNT_8_BIT;
		multi_sampling_create_info.minSampleShading = 1.f;
		multi_sampling_create_info.pSampleMask = null;
		multi_sampling_create_info.alphaToCoverageEnable = VK_FALSE;
		multi_sampling_create_info.alphaToOneEnable = VK_FALSE;
		return *this;
	}

	auto set_color_attachment_format(const VkFormat format) -> PipelineBuilder& {
		color_attachment_format = format;
		rendering_create_info.colorAttachmentCount = 1;
		rendering_create_info.pColorAttachmentFormats = &color_attachment_format;
		return *this;
	}

	auto set_depth_attachment_format(const VkFormat format) -> PipelineBuilder& {
		rendering_create_info.depthAttachmentFormat = format;
		return *this;
	}

	auto disable_depth_test() -> PipelineBuilder& {
		depth_stencil_create_info.depthTestEnable = VK_FALSE;
		depth_stencil_create_info.depthWriteEnable = VK_FALSE;
		depth_stencil_create_info.depthCompareOp = VK_COMPARE_OP_NEVER;
		depth_stencil_create_info.depthBoundsTestEnable = VK_FALSE;
		depth_stencil_create_info.stencilTestEnable = VK_FALSE;
		depth_stencil_create_info.front = {};
		depth_stencil_create_info.back = {};
		depth_stencil_create_info.minDepthBounds = 0.f;
		depth_stencil_create_info.maxDepthBounds = 1.f;
		return *this;
	}

	auto enable_depth_test(const bool depth_write_enabled, const VkCompareOp compare_op = VK_COMPARE_OP_GREATER_OR_EQUAL) -> PipelineBuilder& {
		depth_stencil_create_info.depthTestEnable = VK_TRUE;
		depth_stencil_create_info.depthWriteEnable = depth_write_enabled;
		depth_stencil_create_info.depthCompareOp = compare_op;
		depth_stencil_create_info.stencilTestEnable = VK_FALSE;
		depth_stencil_create_info.front = {};
		depth_stencil_create_info.back = {};
		depth_stencil_create_info.minDepthBounds = 0.f;
		depth_stencil_create_info.maxDepthBounds = 1.f;
		return *this;
	}

	auto enable_blending_additive() -> PipelineBuilder& {
		color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		color_blend_attachment.blendEnable = VK_TRUE;
		color_blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		color_blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
		color_blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
		color_blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		color_blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		color_blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
		return *this;
	}

	auto enable_blending_alpha() -> PipelineBuilder& {
		color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		color_blend_attachment.blendEnable = VK_TRUE;
		color_blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		color_blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		color_blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
		color_blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		color_blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		color_blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
		return *this;
	}

	auto disable_blending() -> PipelineBuilder& {
		color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		color_blend_attachment.blendEnable = VK_FALSE;
		return *this;
	}

	[[nodiscard]] auto build(VkDevice device) const -> VkPipeline {
		// Make viewport state from out stored viewport and scissor.
		// @NOTE: No support for multiple viewports or scissors at the moment.
		const VkPipelineViewportStateCreateInfo viewport_state_create_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1,
			.pViewports = null,// TODO
			.scissorCount = 1,
			.pScissors = null,// TODO
		};

		const VkPipelineColorBlendStateCreateInfo color_blending_create_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.logicOpEnable = VK_FALSE,
			.logicOp = VK_LOGIC_OP_COPY,
			.attachmentCount = 1,
			.pAttachments = &color_blend_attachment,
		};

		const VkPipelineVertexInputStateCreateInfo vertex_input_create_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			// TODO
		};

		static constexpr VkDynamicState DYNAMIC_STATES[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, };

		const VkPipelineDynamicStateCreateInfo dynamic_state_create_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
			.dynamicStateCount = std::size(DYNAMIC_STATES),
			.pDynamicStates = DYNAMIC_STATES,
		};

		const VkGraphicsPipelineCreateInfo graphics_pipeline_create_info{
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.pNext = &rendering_create_info,// Dynamic rendering extension.
			.stageCount = static_cast<u32>(shader_stages.size()),
			.pStages = shader_stages.data(),
			.pVertexInputState = &vertex_input_create_info,
			.pInputAssemblyState = &input_assembly_create_info,
			.pViewportState = &viewport_state_create_info,
			.pRasterizationState = &rasterization_create_info,
			.pMultisampleState = &multi_sampling_create_info,
			.pDepthStencilState = &depth_stencil_create_info,
			.pColorBlendState = &color_blending_create_info,
			.pDynamicState = &dynamic_state_create_info,
			.layout = pipeline_layout,
		};

		VkPipeline pipeline;
		if (vkCreateGraphicsPipelines(device, null, 1, &graphics_pipeline_create_info, null, &pipeline) != VK_SUCCESS) [[unlikely]] {
			WARN("Failed to create graphics pipeline!");
			return null;
		} else {
			return pipeline;
		}
	}

	Array<VkPipelineShaderStageCreateInfo> shader_stages;

	VkPipelineInputAssemblyStateCreateInfo input_assembly_create_info;
	VkPipelineRasterizationStateCreateInfo rasterization_create_info;
	VkPipelineColorBlendAttachmentState color_blend_attachment;
	VkPipelineMultisampleStateCreateInfo multi_sampling_create_info;
	VkPipelineLayout pipeline_layout;
	VkPipelineDepthStencilStateCreateInfo depth_stencil_create_info;
	VkPipelineRenderingCreateInfo rendering_create_info;
	VkFormat color_attachment_format;
};
}
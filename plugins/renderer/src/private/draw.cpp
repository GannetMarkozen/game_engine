#include "draw.hpp"
#include "vk_utils.hpp"
#include "vulkan/vulkan_core.h"

#include <SDL.h>

#include <glm/glm.hpp>
#include <glm/ext.hpp>

namespace renderer {
NOINLINE static auto draw_opaque_geometry(
	ExecContext& context,
	const vkutils::GpuSceneData& scene_data,
	Frame& current_frame,
	const GlobalResources& resources,
	const VkCommandBuffer cmd,
	Query<
		const Transform,
		const SharedPtr<Mesh>
	>& primitive_query
) -> void {
	const VkRenderingAttachmentInfo color_attachment{
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = resources.draw_image.image_view,
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	};

	const VkRenderingAttachmentInfo depth_attachment{
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = resources.depth_image.image_view,
		.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,// Clear existing values (otherwise bad things will happen).
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue{
			.depthStencil{
				// Zero-out depth.
				.depth = 0.f,
			},
		}
	};

	const VkRenderingInfo render_info{
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea{
			.offset{0, 0},
			.extent{resources.draw_image.extent.width, resources.draw_image.extent.height},
		},
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color_attachment,
		.pDepthAttachment = &depth_attachment,

		// Unused for now.
		.pStencilAttachment = null,
	};

	vkCmdBeginRendering(cmd, &render_info);

	// Set dynamic viewport and scissor.
	const VkViewport viewport{
		.x = 0,
		.y = 0,
		.width = static_cast<f32>(resources.draw_image.extent.width),
		.height = static_cast<f32>(resources.draw_image.extent.height),
		.minDepth = 0.f,
		.maxDepth = 1.f,
	};

	const VkRect2D scissor{
		.offset{0, 0},
		.extent{resources.draw_image.extent.width, resources.draw_image.extent.height},
	};

	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);

#if 0
	// Allocate a new buffer for scene data.
	// @NOTE: Suboptimal. Should just store a scene data buffer in the globals.
	const vkutils::Buffer scene_data_buffer = vkutils::create_buffer(resources.allocator, sizeof(vkutils::GpuSceneData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	current_frame.deletion_queue.enqueue([&resources, scene_data_buffer] {
		vkutils::destroy_buffer(resources.allocator, scene_data_buffer);
	});

	void* scene_data_allocation;
	VK_VERIFY(vmaMapMemory(resources.allocator, scene_data_buffer.allocation, &scene_data_allocation));

	std::construct_at(static_cast<vkutils::GpuSceneData*>(scene_data_allocation), scene_data);

	vmaUnmapMemory(resources.allocator, scene_data_buffer.allocation);

	// Create a descriptor set that binds the buffer and update it. (Will automatically freed on this frame interval).
	const auto global_descriptor_set = current_frame.descriptors.allocate(resources.device, resources.scene_data_descriptor_layout);

	vkutils::DescriptorWriter writer;
#endif

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resources.pipelines.mesh_pipeline);

	primitive_query.for_each(context, [&](const Entity& entity, const Transform& transform, const SharedPtr<const Mesh>& mesh) {
		// Bind a texture.
		const auto image_set = current_frame.descriptors.allocate(resources.device, resources.single_image_descriptor_layout);
		{
			vkutils::DescriptorWriter writer;
			writer.write_image(0, resources.error_checkerboard_image.image_view, resources.sampler_nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			writer.update_set(resources.device, image_set);
		}

		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resources.pipelines.mesh_pipeline_layout, 0, 1, &image_set, 0, null);

		//glm::mat4 view = glm::translate(glm::vec3{0.f, 0.f, -5.f});

		const glm::mat4 model_to_world = glm::rotate(glm::radians((static_cast<f32>(0.f) / 120.f) * 3.14f), glm::vec3{0.f, 0.f, 1.f});

		const vkutils::GpuDrawPushConstants push_constants{
			.render_matrix = scene_data.proj * model_to_world * scene_data.view,
			//.render_matrix = projection * model_to_world * view,
			.vertex_buffer = mesh->buffer.vertex_buffer_address,
		};

		vkCmdPushConstants(cmd, resources.pipelines.mesh_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(vkutils::GpuDrawPushConstants), &push_constants);
		vkCmdBindIndexBuffer(cmd, mesh->buffer.index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);

		vkCmdDrawIndexed(cmd, mesh->surfaces[0].count, 1, mesh->surfaces[0].start_index, 0, 0);
	});

	vkCmdEndRendering(cmd);
}

// @TODO: Break this up into handling events, opaque pass, translucent pass, etc.
auto draw(
	ExecContext& context,
	Res<const GlobalResources> resources,
	Res<PendingFrames> frames,
	Res<res::RequestExit> request_exit,
	Res<IsRendering> is_rendering,
	Query<
		const Transform,
		const Camera
	>& camera_query,
	Query<
		const Transform,
		const SharedPtr<Mesh>
	>& primitive_query,
	u64& current_frame
) -> void {
	// Must initialize resources and draw on the same thread! (for now atleast)
	ASSERT(thread::is_in_main_thread());

	WARN("Drawing");

	// Handle SDL events.
	// @NOTE: Draw should not be the thing handling SDL events probably.
	{
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			switch (event.type) {
			case SDL_QUIT:
				request_exit->value = true;
				break;

			case SDL_WINDOWEVENT:
				switch (event.window.event) {
				case SDL_WINDOWEVENT_MINIMIZED:
					is_rendering->value = false;
					break;

				case SDL_WINDOWEVENT_RESTORED:
					is_rendering->value = true;
					break;
				}
			}
		}

		// TMP: Throttle when not rendering.
		if (!is_rendering->value) {
			std::this_thread::sleep_for(std::chrono::milliseconds{10});
			return;
		}

		// Only supports a single camera for now (the primary camera).
		const auto found_camera = camera_query.try_get_single(context);
		if (!found_camera) {
			WARN("No camera in scene!");
			return;
		}

		const auto& [camera_entity, camera_transform, camera] = *found_camera;
		glm::mat4 view = glm::toMat4(camera_transform.rotation) * glm::translate(camera_transform.translation);
		glm::mat4 projection = glm::perspective(glm::radians(camera.fov), static_cast<f32>(resources->draw_image.extent.width) / static_cast<f32>(resources->draw_image.extent.height), camera.clipping_plane.far, camera.clipping_plane.near);

		// Invert the Y direction on projection matrix so that we don't render things upside down (flipped from OpenGL).
		projection[1][1] *= -1.f;

		const vkutils::GpuSceneData scene_data{
			.view = view,
			.proj = projection,
			.view_proj = projection * view,
			.ambient_color{1.f, 0.f, 0.f, 1.f},
			.sunlight_direction{0.f, 1.f, 0.f, 1.f},
			.sunlight_color{0.f, 1.f, 0.f, 1.f},
		};

		const auto frame_interval = frames->current_frame % NUM_FRAMES_IN_FLIGHT;
		auto& current_frame = (*frames)[frame_interval];

		ASSERT(current_frame.render_fence);

		// Wait until GPU has finished rendering the previous frame before attempting to draw the next frame.
		VK_VERIFY(vkWaitForFences(resources->device, 1, &current_frame.render_fence, true, UINT64_MAX));
		VK_VERIFY(vkResetFences(resources->device, 1, &current_frame.render_fence));// Reset after waiting.

		// Flush transient frame data.
		current_frame.deletion_queue.flush();
		current_frame.descriptors.clear_pools(resources->device);

		u32 swapchain_image_index;
		const VkResult acquire_next_image_index_result = vkAcquireNextImageKHR(resources->device, resources->swapchain.swapchain, UINT64_MAX, current_frame.swapchain_semaphore, null, &swapchain_image_index);
		if (acquire_next_image_index_result == VK_ERROR_OUT_OF_DATE_KHR) {
			WARN("Out of date swapchain!");
			return;
		} else {
			ASSERT(acquire_next_image_index_result == VK_SUCCESS);
		}

		// Reset command buffer.
		const VkCommandBuffer cmd = current_frame.command_buffer;
		ASSERT(cmd);

		VK_VERIFY(vkResetCommandBuffer(cmd, 0));

		// Begin "render pass" (using dynamic rendering).
		const VkCommandBufferBeginInfo cmd_begin_info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};

		VK_VERIFY(vkBeginCommandBuffer(cmd, &cmd_begin_info));

		// Draw geometry to a separate image. Transition draw image layout for optimal drawing.
		vkutils::transition_image_layout(cmd, resources->draw_image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

		// Transition depth buffer to optimal format.
		vkutils::transition_image_layout(cmd, resources->depth_image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

		draw_opaque_geometry(context, scene_data, current_frame, *resources, cmd, primitive_query);

		// Transition draw image and swapchain image to optimal layouts to transfer the draw image to the swapchain image.
		vkutils::transition_image_layout(cmd, resources->draw_image.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		vkutils::transition_image_layout(cmd, resources->swapchain.images[swapchain_image_index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		// Copy draw image to swapchain image.
		vkutils::copy_image_to_image(cmd, resources->swapchain.images[swapchain_image_index], resources->draw_image.image, resources->swapchain.extent, VkExtent2D{resources->draw_image.extent.width, resources->draw_image.extent.height});

		// Transition swapchain image to a format optimal for presentation.
		vkutils::transition_image_layout(cmd, resources->swapchain.images[swapchain_image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

		VK_VERIFY(vkEndCommandBuffer(cmd));

		// Submit.
		const VkCommandBufferSubmitInfo cmd_submit_info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
			.commandBuffer = cmd,
		};

		const VkSemaphoreSubmitInfo wait_info{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			.semaphore = current_frame.swapchain_semaphore,
			.value = 1,
			.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
			.deviceIndex = 0,// ???
		};

		const VkSemaphoreSubmitInfo signal_info{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			.semaphore = current_frame.render_semaphore,
			.value = 1,
			.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
			.deviceIndex = 0,
		};

		const VkSubmitInfo2 submit_info{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
			.waitSemaphoreInfoCount = 1,
			// Must wait for the swapchain to become available before submitting.
			.pWaitSemaphoreInfos = &wait_info,
			.commandBufferInfoCount = 1,
			.pCommandBufferInfos = &cmd_submit_info,
			.signalSemaphoreInfoCount = 1,
			// When this frame interval is encountered again. Must wait for the previous frame to finish rendering before continuing.
			.pSignalSemaphoreInfos = &signal_info,
		};

		VK_VERIFY(vkQueueSubmit2(resources->graphics_queue, 1, &submit_info, current_frame.render_fence));

		// Present the swapchain.
		const VkPresentInfoKHR present_info{
			.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,

			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &current_frame.render_semaphore,

			.swapchainCount = 1,
			.pSwapchains = &resources->swapchain.swapchain,

			.pImageIndices = &swapchain_image_index,
		};

		// Can fail due to window resizing or other reasons.
		const VkResult present_result = vkQueuePresentKHR(resources->graphics_queue, &present_info);
		if (present_result == VK_ERROR_OUT_OF_DATE_KHR) {
			WARN("Out of date KHR 2!");
			// @TODO: Recreate swapchain.
			return;
		} else {
			ASSERT(present_result == VK_SUCCESS);
		}

		// Increment the current frame index.
		++frames->current_frame;
	}
}
}
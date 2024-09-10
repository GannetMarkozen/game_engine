#include "renderer_plugin.hpp"

#include <filesystem>

#include "ecs/app.hpp"
#include "gameplay_framework.hpp"

#include "init.hpp"
#include "draw.hpp"

#include "vk_utils.hpp"

namespace group {
struct TmpInitDefaults {};
}

auto tmp_init_defaults(
	ExecContext& context,
	Res<const renderer::GlobalResources> resources
) -> void {
	// Spawn camera.
	context.spawn_entity(
		Transform{
			.translation{0.f, 0.f, -5.f},
		},
		Camera{}
	);

	context.spawn_entity(
		Transform{
			.translation{0.f, 0.f, 1.f},
		},
		vkutils::load_gltf_meshes(ASSET_PATH("basicmesh.glb"), resources->device, resources->allocator, resources->graphics_queue, resources->immediate_submitter)->at(2)
	);
}

auto RendererPlugin::init(App& app) -> void {
	app
		.register_group<group::RenderFrame>(Ordering{
			.within = GroupId::invalid_id(),
		})
		.register_group<group::renderer::Init>(Ordering{
			.within = GroupId::invalid_id(),
		})
		.register_group<group::renderer::Deinit>(Ordering{
			.within = GroupId::invalid_id(),
			// @HACK: No explicit ordering between RenderFrame and Shutdown events (should add that).
			.after = GroupMask::make<group::renderer::Init>(),
		})
		.register_group<group::renderer::Extract>(Ordering{
			.within = get_group_id<group::RenderFrame>(),
			.after = GroupMask::make<group::Movement>(),
		})
		.register_group<group::renderer::Draw>(Ordering{
			.within = get_group_id<group::RenderFrame>(),
			.after = GroupMask::make<group::renderer::Extract>(),
		})
		.register_resource<renderer::GlobalResources>()
		.register_resource<renderer::GlobalDeletionQueue>()
		.register_resource<renderer::PendingFrames>()
		.register_resource(window_config)
		.register_resource<renderer::IsRendering>()
		.register_system<renderer::init>(SystemDesc{
			.group = get_group_id<group::renderer::Init>(),
			.event = get_event_id<event::OnInit>(),
			.priority = Priority::HIGH,
			.thread = Thread::MAIN,// Initialization and deinitialization must run on the same thread!
		})
		.register_system<renderer::deinit>(SystemDesc{
			.group = get_group_id<group::renderer::Deinit>(),
			.event = get_event_id<event::OnShutdown>(),
			.thread = Thread::MAIN,// Initialization and deinitialization must run on the same thread!
		})
		.register_system<renderer::draw>(SystemDesc{
			.group = get_group_id<group::renderer::Draw>(),
			.event = get_event_id<event::OnUpdate>(),
			.priority = Priority::HIGH,
			.thread = Thread::MAIN,// Draw must run on the main thread for now.
		})
		// @TMP
		.register_group<group::TmpInitDefaults>(Ordering{
			.after = GroupMask::make<group::renderer::Init>(),
		})
		.register_system<tmp_init_defaults>(SystemDesc{
			.group = get_group_id<group::TmpInitDefaults>(),
			.event = get_event_id<event::OnInit>(),
			.thread = Thread::MAIN,
		});
}
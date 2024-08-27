#include "gameplay_framework.hpp"
#include "game_loop.hpp"
#include "ecs/app.hpp"

auto GameplayFrameworkPlugin::init(App& app) const -> void {
	app
		.register_group<group::EndGameFrame>(Ordering{
			.within = GroupId::invalid_id(),
			.after = GroupMask::make<group::GameFrame>(),
		})
		.register_group<group::Movement>()// @TODO: Implement.
		// Kicks off OnUpdate loop after OnInit event.
		.register_system<gameplay_framework::BeginUpdateLoopSystem>(SystemDesc{
			.group = get_group_id<group::EndGameFrame>(),
			.event = get_event_id<event::OnInit>(),
			.priority = Priority::HIGH,
		})
		// Continuously dispatches OnUpdate event while res::RequestExit::value == false.
		.register_system<gameplay_framework::EndFrameSystem>(SystemDesc{
			.group = get_group_id<group::EndGameFrame>(),
			.event = get_event_id<event::OnUpdate>(),
			.priority = Priority::HIGH,
		})
		.register_resource<res::RequestExit>();
}
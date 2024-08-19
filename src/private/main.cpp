#include "defines.hpp"
#include "ecs/ids.hpp"
#include "threading/task.hpp"
#include "threading/thread_safe_types.hpp"
#include <chrono>
#include <mutex>
#include "ecs/world.hpp"
#include "ecs/app.hpp"

struct Vec3 {
	f32 x, y, z;
};

template <usize I>
struct Group {};

template <usize I>
struct SpawningSystem {
	static inline constinit Atomic<u32> execution_count = 0;

	[[nodiscard]] static auto get_access_requirements() -> ecs::AccessRequirements {
		return {
			//.writes = ecs::CompMask::make<Vec3>(),
		};
	}

	FORCEINLINE auto execute(ecs::ExecContext& context) -> void {
		WARN("EXECUTING {} {}", I, execution_count++);

		++execution_count;

		using namespace ecs;

		ASSERT(&context.world);

#if 01
		//if constexpr (I == 0)
		for (usize i = 0; i < 100; ++i) {
			const Entity entity = context.world.spawn_entity(
				Vec3{
					.x = static_cast<f32>(i),
					.y = 420,
					.z = 69,
				}
			);
		}
#else
		const Entity entity = context.world.spawn_entity(
				Vec3{
					.x = 100,
					.y = 420,
					.z = 69,
				}
			);
#endif
		context.world.is_pending_destruction = true;
	}
};

struct LoopSystem {
[[nodiscard]] static auto get_access_requirements() -> ecs::AccessRequirements { return { .writes = ecs::CompMask::make<Vec3>(), }; }

FORCEINLINE auto execute(ecs::ExecContext& context) -> void {
	//std::this_thread::sleep_for(std::chrono::seconds{2});

#if 0
	ASSERTF(SpawningSystem<0>::execution_count.load(std::memory_order_relaxed) == SpawningSystem<1>::execution_count.load(std::memory_order_relaxed),
		"Count mismatch! {} != {}!", SpawningSystem<0>::execution_count.load(std::memory_order_relaxed), SpawningSystem<1>::execution_count.load(std::memory_order_relaxed));
#endif
	context.world.dispatch_event<ecs::event::OnInit>();
}
};

struct EndFrameGroup {};

auto main() -> int {
	using namespace ecs;

	App::build()
		.register_group<Group<0>>(Ordering{
			.before = GroupMask::make<Group<1>>(),
		})
		.register_group<Group<1>>()
		.register_system<SpawningSystem<0>>(SystemDesc{
			//.group = get_group_id<Group<0>>(),
			.event = get_event_id<event::OnInit>(),
		})
		#if 0
		.register_system<SpawningSystem<1>>(SystemDesc{
			//.group = get_group_id<Group<1>>(),
			.event = get_event_id<event::OnInit>(),
		})
		#endif
		.register_group<EndFrameGroup>(Ordering{
			.within = GroupId::invalid_id(),
			.after = GroupMask::make<group::GameFrame>(),
		})
		.register_system<LoopSystem>(SystemDesc{
			.group = get_group_id<EndFrameGroup>(),
			.event = get_event_id<event::OnInit>(),
		})
		.run();
}
#include "defines.hpp"
#include "ecs/ids.hpp"
#include "threading/task.hpp"
#include "threading/thread_safe_types.hpp"
#include <chrono>
#include <mutex>
#include "ecs/world.hpp"
#include "ecs/app.hpp"
#include "utils.hpp"
#include "ecs/query.hpp"

Atomic<ecs::SystemId> executing_system = ecs::SystemId::invalid_id();

struct Vec3 {
	f32 x, y, z;
};

struct Name {
	String name;
};

template <usize I>
struct Group {};

template <usize I>
struct SpawningSystem {
	static inline constinit Atomic<u32> execution_count = 0;

	[[nodiscard]] static auto get_access_requirements() -> ecs::AccessRequirements {
		return {
			.writes = ecs::CompMask::make<Vec3>(),
		};
	}

	FORCEINLINE auto execute(ecs::ExecContext& context) -> void {

		const auto previous = executing_system.exchange(ecs::get_system_id<SpawningSystem<I>>());
		ASSERTF(!previous.is_valid(), "Attempted to execute {} while {} was executing!", utils::get_type_name<SpawningSystem<I>>(), ecs::get_type_info(previous).name);

		//std::this_thread::sleep_for(std::chrono::microseconds{30});

		using namespace ecs;

		ASSERT(&context.world);

#if 01
		//if constexpr (I == 0)
		#if 0
		for (usize i = 0; i < 100; ++i) {
			const Entity entity = context.world.spawn_entity(
				Vec3{
					.x = static_cast<f32>(i),
					.y = 420,
					.z = 69,
				}
			);
		}
		#endif

		//task::parallel_for(100, [&](const usize i) {
		for (usize i = 0; i < 100; ++i) {
			if (i % 2 == 0) {
				context.world.spawn_entity(
					Vec3{
						.x = static_cast<f32>(i),
						.y = 420.f,
						.z = 69.f,
					}
				);
			} else {
				context.world.spawn_entity(
					Name{
						.name = fmt::format("Bill {}", i),
					}
				);
			}
		}
		//});
#elif 0
		const Entity entity = context.world.spawn_entity(
				Vec3{
					.x = 100,
					.y = 420,
					.z = 69,
				}
			);
#endif

		//WARN("EXECUTING {}", __PRETTY_FUNCTION__);

		executing_system.store(SystemId::invalid_id());
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
		//context.world.dispatch_event<ecs::event::OnInit>();

		//WARN("EXECUTING {}", __PRETTY_FUNCTION__);

		if (++count < 100) {
			context.world.dispatch_event<ecs::event::OnInit>();
		} else {
			usize count_1 = 0;
			usize count_2 = 0;
			query.for_each(context.world, [&](const Entity& entity, const Vec3& vec) {
				fmt::println("{}: {{{} {} {}}}", entity, vec.x, vec.y, vec.z);
				++count_1;
			});

			other_query.for_each(context.world, [&](const Entity& entity, const Name& name) {
				fmt::println("{}: {}", entity, name.name);
				++count_2;
			});

			fmt::println("Count1 == {}", count_1);
			fmt::println("Count2 == {}", count_2);

			context.world.is_pending_destruction = true;
			task::pending_shutdown = true;
		}
	}

	usize count = 0;
	ecs::Query<const Vec3> query;
	ecs::Query<const Name> other_query;
};

template <usize I>
struct SomeSystem {
	[[nodiscard]] static auto get_access_requirements() -> ecs::AccessRequirements { return {}; }

	FORCEINLINE auto execute(ecs::ExecContext& context) -> void {
		//WARN("EXECUTING {}", __PRETTY_FUNCTION__);

#if 0
		task::parallel_for(100, [&](const usize i) {
			std::this_thread::sleep_for(std::chrono::nanoseconds{50});
		});
#endif
	}

	
};

struct EndFrameGroup {};

template <usize I>
struct SomeGroup {};

auto main() -> int {
	using namespace ecs;

	auto app = App::build();

	const auto register_system = [&]<usize I>() {
		if constexpr (I == 0) {
			app.register_group<SomeGroup<I>>();
		} else {
			app.register_group<SomeGroup<I>>(Ordering{
				.after = GroupMask::make<SomeGroup<I - 1>>(),
			});
		}

		app.register_system<SomeSystem<I>>(SystemDesc{
			.group = get_group_id<SomeGroup<I>>(),
			.event = get_event_id<event::OnInit>(),
		});
	};

		app
		.register_group<Group<0>>(Ordering{
			.within = get_group_id<group::GameFrame>(),
			.before = GroupMask::make<Group<1>>(),
		})
		.register_group<Group<1>>(Ordering{
			.within = get_group_id<group::GameFrame>(),
		})
		.register_system<SpawningSystem<0>>(SystemDesc{
			.group = get_group_id<Group<0>>(),
			.event = get_event_id<event::OnInit>(),
		})
		#if 01
		.register_system<SpawningSystem<1>>(SystemDesc{
			.group = get_group_id<Group<1>>(),
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
		});

#if 01
		utils::make_index_sequence_param_pack<10>([&]<usize... Is>() {
			(register_system.operator()<Is>(), ...);
		});
#endif
		app.run();
		//.run();
}
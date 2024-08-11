#include "core_include.hpp"
#include "defines.hpp"
#include "fmt/core.h"
#include "static_reflection.hpp"
#include "types.hpp"
#include "utils.hpp"



struct SomeOtherStruct {
	f64 double_value = 20.0;
};

struct SomeOtherParentStruct {
	f32 some_other_parent_struct_thing = 10.f;
};

template <>
struct Reflect<SomeOtherParentStruct> {
	using Members = Members<
		DECLARE_MEMBER(&SomeOtherParentStruct::some_other_parent_struct_thing)
	>;
};

struct SomeParentStruct : public SomeOtherParentStruct {
	f32 some_parent_struct_thing = 420.69f;
};

template <>
struct Reflect<SomeParentStruct> {
	using Parent = SomeOtherParentStruct;

	using Members = Members<
		DECLARE_MEMBER(&SomeParentStruct::some_parent_struct_thing)
	>;
};

template <>
struct Reflect<SomeOtherStruct> {
	using Members = Members<
		Member<"double_value", &SomeOtherStruct::double_value>
	>;
};

struct alignas(CACHE_LINE_SIZE) Number {
	i32 number;
};

struct SomeStruct : public SomeParentStruct {
	f32 member = 10;
	i32 value = 999;
	Map<String, SomeOtherStruct> map = { {"Something", {420}}, {"SomethingElse", {69}} };
};

template <>
struct Reflect<SomeStruct> {
	using Parent = SomeParentStruct;

	using Members = Members<
		DECLARE_MEMBER(&SomeStruct::member, "Attr"),
		DECLARE_MEMBER(&SomeStruct::value, {"AttributeWithValue", Number{10}}, {"Fortnite", Number{69}}),
		DECLARE_MEMBER(&SomeStruct::map, "SomeRandomAttribute")
	>;
};

#include <sstream>
#include <chrono>
#include <thread>
#include "serialization.hpp"

#include "ecs/archetype.hpp"

namespace some_int_namespace_int {
	template <typename T>
	struct SomeType {};
}

struct alignas(CACHE_LINE_SIZE) TestStruct {
	u8 data[CACHE_LINE_SIZE];

	static inline i32 constructed_counter = 0;
	static inline i32 destructed_counter = 0;

	TestStruct() {
		++constructed_counter;
	}

	TestStruct(TestStruct&& other) noexcept {
		++constructed_counter;
	}

	~TestStruct() {
		++destructed_counter;
	}
};

template <>
struct IsTriviallyRelocatable<TestStruct> {
	static constexpr bool VALUE = true;
};

#include "ecs/entity_list.hpp"
#include "threading/thread_safe_types.hpp"
#include "ecs/app.hpp"
#include "ecs/world.hpp"
#include "ecs/query.hpp"


#include <thread>
#include <chrono>

template <usize I> struct Group {};
template <usize I> struct Comp {};

Atomic<u32> count = 0;

struct SomeComp {
	String name = "Hello there I am SomeComp";
};


template <usize I>
struct System {
	[[nodiscard]] FORCEINLINE static constexpr auto get_access_requirements() {
		return ecs::AccessRequirements{
			//.writes = ecs::CompMask::make<SomeComp>(),
			.reads = ecs::CompMask::make<SomeComp>(),
		};
	}

	FORCEINLINE auto execute(ecs::World& world) -> void {
		++this_count;
		const auto current_count = ++count;
		fmt::println("Executing System<{}> on thread {}. Count == {}! ThisCount == {}!", I, thread::get_this_thread_id().get_value(), current_count, this_count);
		//std::this_thread::sleep_for(std::chrono::milliseconds{10});

		if (current_count == 13) {
			world.is_pending_destruction = true;
			task::pending_shutdown = true;
		}

		usize some_count = 0;
		query.for_each(world, [&](const Entity& entity, const SomeComp& comp) {
			++some_count;
			//fmt::println("{}: {}: {} {}", ++some_count, static_cast<const void*>(&comp), comp.name, entity);
		});
		//fmt::println("Thing for {}", some_count);
	}

	u32 this_count = 0;

	ecs::Query<SomeComp> query;
};

u32 some_struct_construct_count = 0;
u32 some_struct_destruct_count = 0;
struct PrintStruct {
	PrintStruct() {
		fmt::println("Constructed {} at {}", ++some_struct_construct_count, static_cast<void*>(this));
	}

	~PrintStruct() {
		fmt::println("Destructed {} at {}", ++some_struct_destruct_count, static_cast<void*>(this));
	}

	Array<u32> things = {10, 20, 30, 40, 50};
};

struct ConstructEntitiesSystem {
	[[nodiscard]] static auto get_access_requirements() {
		return ecs::AccessRequirements{
			.modifies = ecs::CompMask::make<SomeComp, Comp<0>>(),
		};
	}

	FORCEINLINE auto execute(ecs::World& world) -> void {
		fmt::println("Gonna spawn entities!");

		std::this_thread::sleep_for(std::chrono::seconds{2});

#if 0
		const auto a = world.spawn_entity(SomeComp{});

		const auto b = world.spawn_entity(SomeComp{});
#else
		//world.spawn_entities(2, SomeComp{.name = "Message of the day!"});
#if 01

		for (usize i = 0; i < 10000; ++i) {
			world.spawn_entity(SomeComp{}, SomeStruct{}, Array<u32>{100, 200, 400, 600, 72389290, 29093});
		}
#else
		ecs::Archetype& archetype = world.find_or_create_archetype(ecs::ArchetypeDesc{
			.comps = ecs::CompMask::make<SomeComp>(),
		}).first;

		Array<Entity> entities;
		for (usize i = 0; i < 100; ++i) {
			entities.push_back(world.reserve_entity());
		}

		archetype.add_defaulted_entities(entities);
#endif
#endif
	}
};

auto main() -> int {
	using namespace ecs;

	MpscQueue<Fn<u64()>> queue;

	const auto start = std::chrono::high_resolution_clock::now();

	Array<std::thread> threads;
	for (usize i = 0; i < 32; ++i) {
		threads.push_back(std::thread{[&] {
			for (u64 i = 0; i < 1000; ++i) {
				queue.enqueue([i] { return i; });
			}
		}});
	}

	for (auto& thread : threads) {
		thread.join();
	}

	const auto end = std::chrono::high_resolution_clock::now();

	Optional<Fn<u64()>> value;
	u64 aggregate_value = 0;
	u64 count = 0;
	u64 max = 0;
	u64 min = std::numeric_limits<u64>::max();
	while ((value = queue.dequeue())) {
		const auto actual_value = (*value)();
		aggregate_value += actual_value;
		max = std::max(max, actual_value);
		min = std::min(min, actual_value);
		++count;
	}
	fmt::println("value == {}. count == {}, max == {}, min == {}", aggregate_value, count, max, min);

	fmt::println("duration == {}", std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());

	std::this_thread::sleep_for(std::chrono::seconds{2});

#if 0

	App app;
	const auto register_group = [&]<usize I>() {
		if constexpr (I != 0) {
			app.register_group<Group<I>>(Ordering{
				.after = GroupMask::make<Group<I - 1>>(),
			});
		} else {
			app.register_group<Group<I>>();
		}
	};

	const auto register_system = [&]<usize I, typename Group>(const task::Thread thread = task::Thread::ANY) {
		app.register_system<System<I>>(SystemDesc{
			.group = get_group_id<Group>(),
			.event = get_event_id<event::OnInit>(),
			.thread = thread,
		});
	};

	[&]<usize I>(this auto&& self) -> void {
		static constexpr usize GROUP_INTERVAL = 100;
		if constexpr (I % GROUP_INTERVAL == 0) {
			register_group.operator()<I / GROUP_INTERVAL>();
		}
		register_system.operator()<I, Group<I / GROUP_INTERVAL>>();
		if constexpr (I < 12) {
			self.template operator()<I + 1>();
		}
	}.operator()<0>();

	app.register_group<Group<10>>(Ordering{
		.before = GroupMask::make<Group<0>>(),
	});

	app.register_system<ConstructEntitiesSystem>(SystemDesc{
		.group = get_group_id<Group<10>>(),
		.event = get_event_id<event::OnInit>(),
	});

	app.run();

	GroupArray<u32> things;
	for (GroupId id = 0; id < get_num_groups(); ++id) {
		things[id] = id.get_value();
	}

	GroupArray<u32> other_things = std::move(things);
	fmt::println("Num groups == {}", get_num_groups());
	fmt::println("{}", other_things[GroupId{get_num_groups() - 1}]);

	const auto end = std::chrono::high_resolution_clock::now();

	fmt::println("FINISHED");
	std::this_thread::sleep_for(std::chrono::seconds{2});

	return EXIT_SUCCESS;
#endif
}
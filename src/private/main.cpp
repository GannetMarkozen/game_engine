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


#include <thread>
#include <chrono>

template <usize I> struct Group {};
template <usize I> struct Comp {};

std::atomic<u32> num_systems_running = 0;

template <usize I>
struct System {
	[[nodiscard]] FORCEINLINE static constexpr auto get_access_requirements() {
		return ecs::AccessRequirements{
			.writes = ecs::CompMask::make<Comp<0>>(),
		};
	}

	FORCEINLINE auto execute(ecs::World& world) -> void {
		ASSERT(++num_systems_running == 1);

		fmt::println("Executing System<{}> on thread {}. Count == {}!", I, thread::get_this_thread_id().get_value(), ++count);
		std::this_thread::sleep_for(std::chrono::milliseconds{10});

		if constexpr (I == 6) {
			if (count == 1) {
				world.is_pending_destruction = true;
				task::pending_shutdown = true;
			}
		}

		--num_systems_running;
	}

	i32 count = 0;
};

auto main() -> int {
	#if 01
	using namespace ecs;

	App app;
	#if 0
	app.register_group<Group<1>>();
	app.register_group<Group<2>>(Ordering{
		.after = GroupMask::make<Group<1>>(),
	});
	app.register_group<Group<3>>(Ordering{
		.after = GroupMask::make<Group<1>, Group<2>>(),
	});
	app.register_group<Group<4>>(Ordering{
		.after = GroupMask::make<Group<2>>(),
	});
	app.register_group<Group<5>>(Ordering{
		.before = GroupMask::make<Group<3>>(),
	});
	app.register_group<Group<6>>(Ordering{
		.after = GroupMask::make<Group<1>, Group<2>, Group<3>, Group<4>, Group<5>>(),
	});
	#endif

	const auto register_group = [&]<usize I>() {
		if constexpr (I != 0) {
			app.register_group<Group<I>>(Ordering{
				.after = GroupMask::make<Group<I - 1>>(),
			});
		} else {
			app.register_group<Group<I>>();
		}
	};

	register_group.operator()<1>();
	register_group.operator()<2>();
	register_group.operator()<3>();
	register_group.operator()<4>();
	register_group.operator()<5>();
	register_group.operator()<6>();

	const auto register_system = [&]<usize I, typename Group>(const task::Thread thread = task::Thread::ANY) {
		app.register_system<System<I>>(SystemDesc{
			.group = get_group_id<Group>(),
			.event = get_event_id<event::OnInit>(),
			.thread = thread,
		});
	};

	register_system.operator()<1, Group<1>>();
	register_system.operator()<2, Group<2>>();
	register_system.operator()<3, Group<3>>();
	register_system.operator()<4, Group<4>>();
	register_system.operator()<5, Group<5>>();
	register_system.operator()<6, Group<5>>();

	app.run();

#elif 0

	task::init();

	volatile bool request_exit = false;

	task::enqueue([&] {
		fmt::println("Hello world!");
		task::parallel_for(100, [&](const usize i) {
			fmt::println("Executing {}", i);
		});
		request_exit = true;
	});

	task::do_work([&] { return request_exit; });

	task::deinit();

#else

	ecs::World::do_something();

#endif

#if 0
	usize num_entities_per_chunk;
	{
		Archetype archetype{ArchetypeDesc{.comps = CompMask::make<SomeStruct, SomeOtherStruct, u8, TestStruct, bool>()}};
		//archetype.add_defaulted_entities(404 * 10);

		Array<Entity> entities;
		for (u32 i = 0; i < 1000; ++i) {
			entities.push_back(Entity{i, 69});
		}

		archetype.add_defaulted_entities(entities);
		//archetype.remove_at(0, 1);
		num_entities_per_chunk = archetype.num_entities_per_chunk;

		usize count = 0;
		archetype.for_each<SomeStruct>([&](const Entity& entity, const SomeStruct& some_struct) {
			std::ostringstream stream;
			serialize_json(stream, some_struct);

			fmt::println("[{}]: Entity {} {}\n{}", ++count, entity.get_index(), entity.get_version(), stream.str());
		});

		archetype.remove_at(100, 520);
	}

	fmt::println("Constructed == {}. Destructed == {}", TestStruct::constructed_counter, TestStruct::destructed_counter);

	fmt::println("num per chunk == {}", num_entities_per_chunk);
#endif

#if 0

	EntityList list;

	Array<Entity> entities;

	static constexpr auto COUNT = 5;

	for (usize i = 0; i < COUNT; ++i) {
		entities.push_back(list.reserve_entity());
	}

	for (usize i = 0; i < COUNT; ++i) {
		list.remove_entity(entities[i]);
	}

	for (usize i = 0; i < COUNT; ++i) {
		entities.push_back(list.reserve_entity());
	}

	for (const auto& entity : entities) {
		fmt::println("{}", entity);
	}
#endif

	return EXIT_SUCCESS;
}
#include "core_include.hpp"
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

struct SomeStruct : public SomeParentStruct {
	f32 member = 10;
	i32 value = 999;
	Map<String, SomeOtherStruct> map = { {"Something", {420}}, {"SomethingElse", {69}} };
};

template <>
struct Reflect<SomeStruct> {
	using Parent = SomeParentStruct;

	using Members = Members<
		DECLARE_MEMBER(&SomeStruct::member),
		DECLARE_MEMBER(&SomeStruct::value),
		DECLARE_MEMBER(&SomeStruct::map)
	>;
};

#include <sstream>
#include <chrono>
#include <thread>
#include "serialization.hpp"

#include "threading/task.hpp"
#include "ecs/component.hpp"

#include "rtti.hpp"

auto main() -> int {
	#if 0
	std::ostringstream stream;

	serialize_json(stream, SomeStruct{});

	fmt::print(fmt::fg(fmt::color::yellow), "{}\n", stream.str());

	std::this_thread::sleep_for(std::chrono::seconds{2});
	#endif

#if 0
	const auto start = std::chrono::high_resolution_clock::now();

	task::init();

	volatile bool request_exit = false;

	std::atomic<u32> count = 0;
	task::enqueue([&] {
		task::parallel_for(100, [&](const usize i) {
			for (usize j = 0; j < 1000; ++j) {
				task::parallel_for(1000, [&](const usize i) { ++count; });
			}
			fmt::println("Finished loop {}: {}", i, count.load(std::memory_order_relaxed));
		});

		request_exit = true;
	}, task::Priority::NORMAL, task::Thread::MAIN);

	task::do_work([&] { return request_exit; });

	const auto end = std::chrono::high_resolution_clock::now();

	fmt::println("Exiting. Time == {}", std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());

	std::this_thread::sleep_for(std::chrono::seconds{2});

	task::deinit();
	#endif

	Any something = Any::make<Array<i32>>(Array<i32>{10, 20, 30});
	Any something_else = something;

	const auto& value = something_else.as<Array<i32>>();
	for (usize i = 0; i < value.size(); ++i) {
		fmt::println("something_else[{}] == {}", i, value[i]);
	}

	return EXIT_SUCCESS;
}
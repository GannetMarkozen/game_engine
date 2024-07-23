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

auto main() -> int {
	using namespace ecs;

	usize num_entities_per_chunk;
	{
		Archetype archetype{ArchetypeDesc{.comps = CompMask::make<SomeStruct, SomeOtherStruct, u8, TestStruct, bool>()}};
		archetype.add_defaulted_entities(404 * 10);
		//archetype.remove(10, 10);
		//archetype.remove_from_end(405);
		archetype.remove_at(0, 404 + 1);
		num_entities_per_chunk = archetype.num_entities_per_chunk;
	}

	fmt::println("Constructed == {}. Destructed == {}", TestStruct::constructed_counter, TestStruct::destructed_counter);

	fmt::println("num per chunk == {}", num_entities_per_chunk);

	return EXIT_SUCCESS;
}
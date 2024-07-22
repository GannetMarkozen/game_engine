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

#include "threading/task.hpp"
#include "ecs/component.hpp"

#include "rtti.hpp"

namespace some_int_namespace_int {
	template <typename T>
	struct SomeType {};
}

auto main() -> int {
	const auto& type_info = rtti::get_type_info<SomeStruct>();
	for (const auto& member : type_info.members) {
		fmt::println("{} {}: {}", member.type->name, member.name, member.offset);
		for (const auto& attribute : member.attributes) {
			fmt::println("\t{} {}: {}", attribute.value.get_type()->name, attribute.name, static_cast<const Number*>(attribute.value.get_data())->number);
		}
	}

	return EXIT_SUCCESS;
}
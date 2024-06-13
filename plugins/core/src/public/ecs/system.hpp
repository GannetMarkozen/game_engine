#pragma once

#include "defines.hpp"
#include "query.hpp"
#include "concurrency/task_graph.hpp"

struct SystemGroupInfo {
	StringView name;
	u8 num_entries;
};

template<concepts::Enum T>
struct GroupTraitsBase {
	static consteval fn get_name() -> StringView {
		return get_type_name<T>();
	}

	static consteval fn get_num_entries() -> u32 {
		return static_cast<u8>(T::COUNT);
	}
};

template<concepts::Enum T>
struct GroupTraits : public GroupTraitsBase<T> {};

namespace ecs {
namespace concepts {
template<typename T>
concept Group = requires {
	{ T::COUNT } -> std::same_as<T>;
} && std::is_enum_v<T> && std::is_same_v<__underlying_type(T), u8>;
}

struct World;

struct SystemGroupId : public IntAlias<u16> {
	using IntAlias<u16>::IntAlias;
};

struct SystemGroup {
	[[nodiscard]] FORCEINLINE fn operator==(const SystemGroup& other) const -> bool { return group == other.group && value == other.value; }
	[[nodiscard]] FORCEINLINE fn operator!=(const SystemGroup& other) const -> bool { return !(*this == other); }

	SystemGroupId group;
	u8 value;
};

namespace impl {
EXPORT_API inline Array<SystemGroupInfo> group_infos;
}

template<concepts::Group T>
inline fn get_group_id() -> SystemGroupId {
	static const SystemGroupId GROUP = [] {
		const usize old_size = impl::group_infos.size();
		ASSERT(old_size < UINT16_MAX);

		using Traits = GroupTraits<T>;
		impl::group_infos.push_back(SystemGroupInfo{
			.name = Traits::get_name(),
			.num_entries = Traits::get_num_entries(),
		});

		return SystemGroupId{static_cast<u16>(old_size)};
	}();

	return GROUP;
}

FORCEINLINE fn get_group(const concepts::Group auto group) -> SystemGroup {
	return SystemGroup{
		.group = get_group_id<std::decay_t<decltype(group)>>(),
		.value = static_cast<u8>(group),
	};
}

FORCEINLINE fn get_group_info(const SystemGroupId id) -> const SystemGroupInfo& {
	return impl::group_infos[id];
}

struct GroupOrdering {
	fn before(const concepts::Group auto... groups) -> GroupOrdering& requires (sizeof...(groups) > 0) {
		auto groups_array = { get_group(groups)... };
		prerequisites.append_range(groups_array);
		return *this;
	}

	fn after(const concepts::Group auto... groups) -> GroupOrdering& requires (sizeof...(groups) > 0) {
		auto groups_array = { get_group(groups)... };
		subsequents.append_range(groups_array);
		return *this;
	}

	Array<SystemGroup> prerequisites;
	Array<SystemGroup> subsequents;
};

struct SystemInfo {
	StringView name = "Unassigned";
	Requirements requirements;
	Optional<SystemGroup> group;// The "group" this system runs within.
	Optional<f32> tick_interval;// Unimplemented.
	task::Priority::Type priority = task::Priority::NORMAL;
	task::Thread::Type thread = task::Thread::ANY;
};

struct alignas(CACHE_LINE_SIZE) SystemBase {
	NON_COPYABLE(SystemBase);

	SystemBase() = default;

	virtual ~SystemBase() = default;
	virtual fn execute(World& world) -> void = 0;
};

namespace concepts {
template<typename T>
concept System = (!std::is_same_v<T, SystemBase> && std::derived_from<T, SystemBase> && std::is_constructible_v<T, Requirements&>);
}
}

namespace std {
template<>
struct hash<ecs::SystemGroup> {
	[[nodiscard]]
	constexpr fn operator()(const ecs::SystemGroup& group) const -> u32 {
		return math::hash_combine(std::hash<IntAlias<u16>>{}(group.group), std::hash<u8>{}(group.value));
	}
};
}
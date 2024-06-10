#pragma once

#include "defines.hpp"
#include "query.hpp"


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

namespace core::ecs {
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

template<concepts::Enum T>
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

FORCEINLINE fn get_group(const concepts::Enum auto group) -> SystemGroup {
	return SystemGroup{
		.group = get_group_id<std::decay_t<decltype(group)>>(),
		.value = static_cast<u8>(group),
	};
}

FORCEINLINE fn get_group_info(const SystemGroupId id) -> const SystemGroupInfo& {
	return impl::group_infos[id];
}

struct GroupOrdering {
	[[nodiscard]]
	static fn before(const concepts::Enum auto... groups) -> GroupOrdering requires (sizeof...(groups) > 0) {
		return GroupOrdering{
			.prerequisites = { get_group(groups)... },
		};
	}

	[[nodiscard]]
	static fn after(const concepts::Enum auto... groups) -> GroupOrdering requires (sizeof...(groups) > 0) {
		return GroupOrdering{
			.subsequents = { get_group(groups)... },
		};
	}

	fn before(const concepts::Enum auto... groups) -> GroupOrdering& requires (sizeof...(groups) > 0) {
		prerequisites.append_range({ get_group(groups)... });
		return *this;
	}

	fn after(const concepts::Enum auto... groups) -> GroupOrdering& requires (sizeof...(groups) > 0) {
		subsequents.append_range({ get_group(groups)... });
		return *this;
	}

	Array<SystemGroup> prerequisites;
	Array<SystemGroup> subsequents;
};

struct SystemInfo {
	StringView name = "None";
	Requirements requirements;
	Optional<SystemGroup> group;// The "group" this system runs within.
	Optional<f32> tick_interval;// Unimplemented.
};

struct alignas(CACHE_LINE_SIZE) SystemBase {
	NON_COPYABLE(SystemBase);

	SystemBase() = delete;
	explicit SystemBase(Requirements& out_requirements);

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
struct hash<core::ecs::SystemGroup> {
	[[nodiscard]]
	constexpr fn operator()(const core::ecs::SystemGroup& group) const -> u32 {
		return math::hash_combine(std::hash<IntAlias<u16>>{}(group.group), std::hash<u8>{}(group.value));
	}
};
}
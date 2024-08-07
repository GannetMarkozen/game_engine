#pragma once

#include "type_registry.hpp"

DECLARE_NAMESPACED_INT_ALIAS(ecs, CompId, u8);
DECLARE_NAMESPACED_INT_ALIAS(ecs, ArchetypeId, u8);
DECLARE_NAMESPACED_INT_ALIAS(ecs, SystemId, u8);
DECLARE_NAMESPACED_INT_ALIAS(ecs, GroupId, u8);
DECLARE_NAMESPACED_INT_ALIAS(ecs, EventId, u8);

namespace ecs {
struct World;

using CompMask = StaticTypeMask<CompId, 128>;
using ArchetypeMask = TypeMask<ArchetypeId>;// Dynamic mask because the number of archetypes is unpredictable at runtime.
using SystemMask = StaticTypeMask<SystemId, 128>;
using GroupMask = StaticTypeMask<GroupId, 64>;
using EventMask = StaticTypeMask<EventId, 64>;

struct AccessRequirements {
	[[nodiscard]] FORCEINLINE constexpr auto can_execute_concurrently_with(const AccessRequirements& other) const -> bool {
		return !(writes & (other.reads | other.writes)) && !(other.writes & (reads | writes)) &&// Check exclusive write access violations.
			!((reads | writes) & other.modifies) && !((other.reads | other.writes) & modifies);// Check structural modifications access violations.
	}

	FORCEINLINE constexpr auto operator|=(const AccessRequirements& other) -> AccessRequirements& {
		reads |= other.reads;
		writes |= other.writes;
		modifies |= other.modifies;
		return *this;
	}

	FORCEINLINE constexpr auto operator&=(const AccessRequirements& other) -> AccessRequirements& {
		reads &= other.reads;
		writes &= other.writes;
		modifies &= other.modifies;
		return *this;
	}

	FORCEINLINE constexpr auto operator^=(const AccessRequirements& other) -> AccessRequirements& {
		reads ^= other.reads;
		writes ^= other.writes;
		modifies ^= other.modifies;
		return *this;
	}

	FORCEINLINE constexpr auto negate() -> AccessRequirements& {
		reads.negate();
		writes.negate();
		modifies.negate();
		return *this;
	}

	[[nodiscard]] FORCEINLINE constexpr friend auto operator|(AccessRequirements a, const AccessRequirements& b) -> AccessRequirements {
		return a |= b;
	}

	[[nodiscard]] FORCEINLINE constexpr friend auto operator&(AccessRequirements a, const AccessRequirements& b) -> AccessRequirements {
		return a &= b;
	}

	[[nodiscard]] FORCEINLINE constexpr friend auto operator^(AccessRequirements a, const AccessRequirements& b) -> AccessRequirements {
		return a ^= b;
	}

	[[nodiscard]] FORCEINLINE constexpr friend auto operator~(AccessRequirements value) -> AccessRequirements {
		return value.negate();
	}

	CompMask reads;// Components with immutable access.
	CompMask writes;// Components with mutable access.
	CompMask modifies;// Components being added / removed / spawned (archetype structural changes).
};

namespace cpts {
template <typename T>
concept System = requires (T t, World& world) {
	t.execute(world);
	{ T::get_access_requirements() } -> std::same_as<AccessRequirements>;
};
}

template <typename T>
[[nodiscard]] FORCEINLINE auto get_comp_id() -> CompId {
	return TypeRegistry<CompId>::get_id<T>();
}

[[nodiscard]] FORCEINLINE auto get_num_comps() -> usize {
	return TypeRegistry<CompId>::get_num_registered_types();
}

template <typename T>
[[nodiscard]] FORCEINLINE auto get_group_id() -> GroupId {
	return TypeRegistry<GroupId>::get_id<T>();
}

[[nodiscard]] FORCEINLINE auto get_num_groups() -> usize {
	return TypeRegistry<GroupId>::get_num_registered_types();
}

template <cpts::System T>
[[nodiscard]] FORCEINLINE auto get_system_id() -> SystemId {
	return TypeRegistry<SystemId>::get_id<T>();
}

[[nodiscard]] FORCEINLINE auto get_num_systems() -> usize {
	return TypeRegistry<SystemId>::get_num_registered_types();
}

template <typename T> requires std::is_empty_v<T>
[[nodiscard]] FORCEINLINE auto get_event_id() -> EventId {
	return TypeRegistry<EventId>::get_id<T>();
}

[[nodiscard]] FORCEINLINE auto get_num_events() -> usize {
	return TypeRegistry<EventId>::get_num_registered_types();
}
}
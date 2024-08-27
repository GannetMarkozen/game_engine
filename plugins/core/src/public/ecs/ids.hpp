#pragma once

#include "type_registry.hpp"

DECLARE_NAMESPACED_INT_ALIAS(ecs, CompId, u16);
DECLARE_NAMESPACED_INT_ALIAS(ecs, ArchetypeId, u32);
DECLARE_NAMESPACED_INT_ALIAS(ecs, SystemId, u16);
DECLARE_NAMESPACED_INT_ALIAS(ecs, GroupId, u16);
DECLARE_NAMESPACED_INT_ALIAS(ecs, EventId, u8);
DECLARE_NAMESPACED_INT_ALIAS(ecs, ResId, u8);

namespace ecs {
struct World;
struct ExecContext;

using CompMask = StaticTypeMask<CompId, 512>;
using ArchetypeMask = TypeMask<ArchetypeId>;// Dynamic mask because the number of archetypes is unpredictable at runtime.
using SystemMask = StaticTypeMask<SystemId, 512>;
using GroupMask = StaticTypeMask<GroupId, 512>;
using EventMask = StaticTypeMask<EventId, 64>;
using ResMask = StaticTypeMask<ResId, 256>;

template <typename T> using CompArray = TypeArray<CompId, T>;
template <typename T> using SystemArray = TypeArray<SystemId, T>;
template <typename T> using GroupArray = TypeArray<GroupId, T>;
template <typename T> using EventArray = TypeArray<EventId, T>;
template <typename T> using ResArray = TypeArray<ResId, T>;

template <typename... Ts> using CompMultiArray = TypeMultiArray<CompId, Ts...>;
template <typename... Ts> using SystemMultiArray = TypeMultiArray<SystemId, Ts...>;
template <typename... Ts> using GroupMultiArray = TypeMultiArray<GroupId, Ts...>;
template <typename... Ts> using EventMultiArray = TypeMultiArray<EventId, Ts...>;
template <typename... Ts> using ResMultiArray = TypeMultiArray<ResId, Ts...>;

#if 0
struct AccessRequirements {
	[[nodiscard]] FORCEINLINE constexpr auto can_execute_concurrently_with(const AccessRequirements& other) const -> bool {
		return !((writes | modifies) & (other.reads | other.writes | other.modifies)) &&
			!((other.writes | other.modifies) & (reads | writes | modifies));
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
#endif

struct AccessRequirements {
	[[nodiscard]] constexpr auto can_execute_concurrently_with(const AccessRequirements& other) const -> bool {
		return !((comps.writes | comps.concurrent_writes) & (other.comps.reads | other.comps.writes)) && !((resources.writes | resources.concurrent_writes) & (other.comps.reads | other.comps.writes)) &&
			!((other.resources.writes | other.resources.concurrent_writes) & (resources.reads | resources.writes)) && !((other.resources.writes | other.resources.concurrent_writes) & (resources.reads | resources.writes));
	}

	[[nodiscard]] constexpr auto get_accessing_comps() const -> CompMask {
		return comps.reads | comps.writes | comps.concurrent_writes;
	}

	[[nodiscard]] constexpr auto get_accessing_resources() const -> ResMask {
		return resources.reads | resources.writes | resources.concurrent_writes;
	}

	constexpr auto operator|=(const AccessRequirements& other) -> AccessRequirements& {
		comps.reads |= other.comps.reads;
		comps.writes |= other.comps.writes;
		comps.concurrent_writes |= other.comps.concurrent_writes;

		resources.reads |= other.resources.reads;
		resources.writes |= other.resources.writes;
		resources.concurrent_writes |= other.resources.concurrent_writes;

		return *this;
	}

	constexpr auto operator&=(const AccessRequirements& other) -> AccessRequirements& {
		comps.reads &= other.comps.reads;
		comps.writes &= other.comps.writes;
		comps.concurrent_writes &= other.comps.concurrent_writes;

		resources.reads &= other.resources.reads;
		resources.writes &= other.resources.writes;
		resources.concurrent_writes &= other.resources.concurrent_writes;

		return *this;
	}

	constexpr auto operator^=(const AccessRequirements& other) -> AccessRequirements& {
		comps.reads ^= other.comps.reads;
		comps.writes ^= other.comps.writes;
		comps.concurrent_writes ^= other.comps.concurrent_writes;

		resources.reads ^= other.resources.reads;
		resources.writes ^= other.resources.writes;
		resources.concurrent_writes ^= other.resources.concurrent_writes;

		return *this;
	}

	[[nodiscard]] friend constexpr auto operator|(AccessRequirements a, const AccessRequirements& b) -> AccessRequirements {
		return a |= b;
	}

	[[nodiscard]] friend constexpr auto operator&(AccessRequirements a, const AccessRequirements& b) -> AccessRequirements {
		return a &= b;
	}

	[[nodiscard]] friend constexpr auto operator^(AccessRequirements a, const AccessRequirements& b) -> AccessRequirements {
		return a ^= b;
	}

	struct {
		CompMask reads, writes, concurrent_writes;
	} comps;

	struct {
		ResMask reads, writes, concurrent_writes;
	} resources;
};

namespace cpts {
template <typename T>
concept System = requires (T t, ExecContext& context) {
	t.execute(context);
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

template <typename T>
[[nodiscard]] FORCEINLINE auto get_res_id() -> ResId {
	return TypeRegistry<ResId>::get_id<T>();
}

[[nodiscard]] FORCEINLINE auto get_num_resources() -> usize {
	return TypeRegistry<ResId>::get_num_registered_types();
}
}
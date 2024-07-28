#pragma once

#include "type_registry.hpp"

DECLARE_NAMESPACED_INT_ALIAS(ecs, CompId, u16);
DECLARE_NAMESPACED_INT_ALIAS(ecs, ArchetypeId, u16);

namespace ecs {
template <typename T>
[[nodiscard]] FORCEINLINE auto get_comp_id() -> CompId {
	return TypeRegistry<CompId>::get_id<T>();
}

using CompMask = StaticTypeMask<CompId, 64>;
}
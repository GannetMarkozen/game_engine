#pragma once

#include "type_registry.hpp"

namespace ecs {
struct ArchetypeId final : public IntAlias<u16> {
	using IntAlias<u16>::IntAlias;
};

struct CompId final : public IntAlias<u16> {
	using IntAlias<u16>::IntAlias;
};

template <typename T>
[[nodiscard]] FORCEINLINE auto get_comp_id() -> CompId {
	return TypeRegistry<CompId>::get_id<T>();
}

using CompMask = StaticTypeMask<CompId, 64>;
}
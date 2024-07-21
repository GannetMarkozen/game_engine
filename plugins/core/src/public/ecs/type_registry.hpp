#pragma once

#include "defines.hpp"

namespace ecs {
namespace impl {
template <typename Tag, std::integral Int>
struct TypeRegistryIncrementer {
	[[nodiscard]] FORCEINLINE static auto increment() -> Int {
		return incrementer++;
	}

private:
	EXPORT_API inline static constinit Int incrementer = 0;
};
}

template <typename T, typename Tag = std::nullptr_t, std::integral Int = u16>
struct TypeRegistry {
	[[nodiscard]] FORCEINLINE static auto get() -> Int {
		return VALUE;
	}

private:
	EXPORT_API inline static const Int VALUE = impl::TypeRegistryIncrementer<Tag, Int>::increment();
};
}
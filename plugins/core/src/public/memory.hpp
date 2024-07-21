#pragma once

#include "defines.hpp"

namespace mem {
constexpr usize DEFAULT_ALIGNMENT = 0;

static_assert(std::same_as<__underlying_type(std::align_val_t), usize>);

[[nodiscard]] FORCEINLINE constexpr auto alloc(const usize size, const usize alignment = DEFAULT_ALIGNMENT) -> void* {
	return ::operator new(size, static_cast<std::align_val_t>(alignment));
}

FORCEINLINE constexpr auto dealloc(void* data, const usize alignment = DEFAULT_ALIGNMENT) -> void {
	return ::operator delete(data, static_cast<std::align_val_t>(alignment));
}

[[nodiscard]] FORCEINLINE constexpr auto resize(void* data, const usize size, const usize alignment = DEFAULT_ALIGNMENT) -> void* {
	return _aligned_realloc(data, size, alignment);
}
}
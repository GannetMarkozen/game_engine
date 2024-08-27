#pragma once

#include "defines.hpp"

namespace mem {
constexpr usize DEFAULT_ALIGNMENT = 8;

static_assert(std::same_as<__underlying_type(std::align_val_t), usize>);

[[nodiscard]] FORCEINLINE constexpr auto alloc(const usize size, const usize alignment = DEFAULT_ALIGNMENT) -> void* {
	if (alignment <= DEFAULT_ALIGNMENT) {
		return malloc(size);
	} else {
		return _aligned_malloc(size, alignment);
	}
}

FORCEINLINE constexpr auto dealloc(void* data, const usize alignment = DEFAULT_ALIGNMENT) -> void {
	if (alignment <= DEFAULT_ALIGNMENT) {
		free(data);
	} else {
		_aligned_free(data);
	}
}

[[nodiscard]] FORCEINLINE constexpr auto resize(void* data, const usize size, const usize alignment = DEFAULT_ALIGNMENT) -> void* {
	if (alignment <= DEFAULT_ALIGNMENT) {
		return realloc(data, size);
	} else {
		return _aligned_realloc(data, size, alignment);
	}
}
}
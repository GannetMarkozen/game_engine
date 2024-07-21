#pragma once

#include "types.hpp"
#include "bitmask.hpp"
#include "type_registry.hpp"

struct CompId final : public IntAlias<u16> {
	using IntAlias<u16>::IntAlias;
};

namespace ecs {
namespace impl {
struct CompTag {};
}

template <typename T>
[[nodiscard]] FORCEINLINE auto get_comp_id() -> CompId {
	return CompId{TypeRegistry<T, impl::CompTag, u16>::get()};
}

struct CompMask {
	[[nodiscard]] FORCEINLINE constexpr auto operator&=(const CompMask& other) -> CompMask& {
		mask &= other.mask;
		return *this;
	}

	[[nodiscard]] FORCEINLINE constexpr auto operator|=(const CompMask& other) -> CompMask& {
		mask |= other.mask;
		return *this;
	}

	[[nodiscard]] FORCEINLINE constexpr auto operator^=(const CompMask& other) -> CompMask& {
		mask ^= other.mask;
		return *this;
	}

	[[nodiscard]] FORCEINLINE friend constexpr auto operator&(const CompMask& a, const CompMask& b) -> CompMask {
		return auto{a} &= b;
	}

	[[nodiscard]] FORCEINLINE friend constexpr auto operator|(const CompMask& a, const CompMask& b) -> CompMask {
		return auto{a} |= b;
	}

	[[nodiscard]] FORCEINLINE friend constexpr auto operator^(const CompMask& a, const CompMask& b) -> CompMask {
		return auto{a} ^= b;
	}

	[[nodiscard]] FORCEINLINE constexpr auto add(const CompId id) -> CompMask& {
		mask[id.get_value()] = true;
		return *this;
	}

	[[nodiscard]] FORCEINLINE constexpr auto has(const CompId id) const -> bool {
		return mask[id.get_value()];
	}

	template <typename T>
	[[nodiscard]] FORCEINLINE auto add() -> CompMask& {
		return add(get_comp_id<T>());
	}

	template <typename T>
	[[nodiscard]] FORCEINLINE auto has() const -> bool {
		return has(get_comp_id<T>());
	}

	BitMask<64> mask;
};
}
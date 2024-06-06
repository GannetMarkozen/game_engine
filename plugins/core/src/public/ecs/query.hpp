#pragma once

#include "../defines.hpp"
#include "types.hpp"
#include "component.hpp"

namespace core::ecs {

struct Requirements {
	template<typename... Ts>
	requires ((!std::is_empty_v<Ts> && ...) && sizeof...(Ts) > 0)
	FORCEINLINE fn reads() -> Requirements& {
		read_mask.add<Ts...>();
		return *this;
	}

	template<typename... Ts>
	requires ((!std::is_empty_v<Ts> && ...) && sizeof...(Ts) > 0)
	FORCEINLINE fn writes() -> Requirements& {
		write_mask.add<Ts...>();
		return *this;
	}

	template<typename... Ts>
	requires (sizeof...(Ts) > 0)
	FORCEINLINE fn includes() -> Requirements& {
		include_mask.add<Ts...>();
		return *this;
	}

	template<typename... Ts>
	requires (sizeof...(Ts) > 0)
	FORCEINLINE fn excludes() -> Requirements& {
		exclude_mask.add<Ts...>();
		return *this;
	}

	fn operator&=(const Requirements& other) -> Requirements& {
		read_mask &= other.read_mask;
		write_mask &= other.write_mask;
		include_mask &= other.include_mask;
		exclude_mask &= other.exclude_mask;
		return *this;
	}

	fn operator|=(const Requirements& other) -> Requirements& {
		read_mask |= other.read_mask;
		write_mask |= other.write_mask;
		include_mask |= other.include_mask;
		exclude_mask |= other.exclude_mask;
		return *this;
	}

	[[nodiscard]]
	FORCEINLINE fn operator&(const Requirements& other) -> Requirements {
		auto copy = *this;
		return copy &= other;
	}

	[[nodiscard]]
	FORCEINLINE fn operator|(const Requirements& other) -> Requirements {
		auto copy = *this;
		return copy |= other;
	}

	[[nodiscard]]
	fn can_execute_concurrently_with(const Requirements& other) const -> bool {
		return !write_mask.has_any_matching_set_bits(other.write_mask) && !write_mask.has_any_matching_set_bits(other.read_mask) && !other.write_mask.has_any_matching_set_bits(read_mask);
	}

	ComponentMask read_mask;
	ComponentMask write_mask;
	ComponentMask include_mask;
	ComponentMask exclude_mask;
};

struct Query {
	Requirements requirements;
};
}
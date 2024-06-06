#pragma once

#include <bit>
#include "defines.hpp"

namespace math {
[[nodiscard]]
FORCEINLINE constexpr fn divide_and_round_down(const std::integral auto dividend, const std::integral auto divider) {
	return dividend / divider;
}

[[nodiscard]]
FORCEINLINE constexpr fn divide_and_round_up(const std::integral auto dividend, const std::integral auto divider) {
	return (dividend + divider - 1) / divider;
}

[[nodiscard]]
FORCEINLINE constexpr fn make_unsigned(const std::signed_integral auto value) -> std::make_unsigned_t<decltype(value)> {
	return static_cast<std::make_unsigned_t<decltype(value)>>(value);
}

template<typename T>
requires std::is_arithmetic_v<T>
FORCEINLINE constexpr fn truncate(const std::floating_point auto value) -> T {
	if constexpr (std::is_floating_point_v<T>) {
		if consteval {
			return static_cast<decltype(value)>(static_cast<u64>(value));
		} else {
			return trunc(value);
		}
	} else {
		return static_cast<T>(value);
	}
}

template<typename T = i32>
requires std::is_arithmetic_v<T>
[[nodiscard]]
FORCEINLINE constexpr fn floor(const std::floating_point auto value) -> T {
	if constexpr (std::is_floating_point_v<T> && std::is_floating_point_v<decltype(value)> && !std::is_constant_evaluated()) {
		return ::floor(value);
	}

	auto trunc_value = truncate<T>(value);
	trunc_value -= (trunc_value > value);
	return trunc_value;
}

template<typename T = i32>
requires std::is_arithmetic_v<T>
[[nodiscard]]
FORCEINLINE constexpr fn ceil(const std::floating_point auto value) -> T {
	if constexpr (std::is_floating_point_v<T> && std::is_floating_point_v<decltype(value)> && !std::is_constant_evaluated()) {
		return ::ceil(value);
	}

	auto trunc_value = truncate<T>(value);
	trunc_value += (trunc_value < value);
	return trunc_value;
}

template<typename T = i32>
requires std::is_arithmetic_v<T>
[[nodiscard]]
FORCEINLINE constexpr fn round(const std::floating_point auto value) -> T {
	if constexpr (std::is_floating_point_v<T> && std::is_floating_point_v<decltype(value)> && !std::is_constant_evaluated()) {
		return ::round(value);
	}

	return floor<T>(value + static_cast<decltype(value)>(0.5));
}

[[nodiscard]]
FORCEINLINE constexpr fn make_unsigned(const std::unsigned_integral auto value) -> decltype(value) {
	return value;
}

[[nodiscard]]
FORCEINLINE constexpr fn count_trailing_zeros(const std::integral auto value) -> u32 {
	return value == 0 ? sizeof(value) * 8 : std::countr_zero(make_unsigned(value));
}

[[nodiscard]]
FORCEINLINE constexpr fn count_leading_zeros(const std::integral auto value) -> u32 {
	return value == 0 ? sizeof(value) * 8 : std::countl_zero(make_unsigned(value));
}

[[nodiscard]]
FORCEINLINE constexpr fn count_set_bits(const std::integral auto value) -> u32 {
	return std::popcount(make_unsigned(value));
}

[[nodiscard]]
FORCEINLINE constexpr fn ceil_log_two(const std::integral auto value) -> u32 {
	static constexpr auto NUM_BITS = sizeof(value) * 8;
	return NUM_BITS - count_leading_zeros(value > 0 ? value - static_cast<decltype(value)>(1) : static_cast<decltype(value)>(1));
}

[[nodiscard]]
FORCEINLINE constexpr fn floor_log_two(const std::integral auto value) -> u32 {
	static constexpr auto NUM_BITS = sizeof(value) * 8;
	return (NUM_BITS - 1) - count_leading_zeros(value > 0 ? value : static_cast<decltype(value)>(1));
}

[[nodiscard]]
FORCEINLINE constexpr fn round_up_to_power_of_two(const std::integral auto value) -> u32 {
	return 1u << ceil_log_two(value);
}

[[nodiscard]]
FORCEINLINE constexpr fn round_down_to_power_of_two(const std::integral auto value) -> u32 {
	return 1u << floor_log_two(value);
}

// The number of bits required to represent this value.
[[nodiscard]]
FORCEINLINE constexpr fn num_bits_required(const std::integral auto value) -> u32 {
	return std::bit_width(make_unsigned(value));
}
}
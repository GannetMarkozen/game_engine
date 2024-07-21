#pragma once

#include <bit>
#include "defines.hpp"

namespace math {
[[nodiscard]] FORCEINLINE constexpr auto divide_and_round_down(const std::integral auto dividend, const std::integral auto divider) {
	return dividend / divider;
}

[[nodiscard]] FORCEINLINE constexpr auto divide_and_round_up(const std::integral auto dividend, const std::integral auto divider) {
	return (dividend + divider - 1) / divider;
}

[[nodiscard]] FORCEINLINE constexpr auto make_unsigned(const std::signed_integral auto value) -> std::make_unsigned_t<decltype(value)> {
	return static_cast<std::make_unsigned_t<decltype(value)>>(value);
}

[[nodiscard]] FORCEINLINE auto make_unsigned(const cpts::Pointer auto value) -> uptr {
	return reinterpret_cast<uptr>(value);
}

template<typename T> requires std::is_arithmetic_v<T>
[[nodiscard]] FORCEINLINE constexpr auto truncate(const std::floating_point auto value) -> T {
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

template<typename T = i32> requires std::is_arithmetic_v<T>
[[nodiscard]] FORCEINLINE constexpr auto floor(const std::floating_point auto value) -> T {
	if constexpr (std::is_floating_point_v<T> && std::is_floating_point_v<decltype(value)> && !std::is_constant_evaluated()) {
		return ::floor(value);
	}

	auto trunc_value = truncate<T>(value);
	trunc_value -= (trunc_value > value);
	return trunc_value;
}

template<typename T = i32> requires std::is_arithmetic_v<T>
[[nodiscard]] FORCEINLINE constexpr auto ceil(const std::floating_point auto value) -> T {
	if constexpr (std::is_floating_point_v<T> && std::is_floating_point_v<decltype(value)> && !std::is_constant_evaluated()) {
		return ::ceil(value);
	}

	auto trunc_value = truncate<T>(value);
	trunc_value += (trunc_value < value);
	return trunc_value;
}

template<typename T = i32> requires std::is_arithmetic_v<T>
[[nodiscard]] FORCEINLINE constexpr auto round(const std::floating_point auto value) -> T {
	if constexpr (std::is_floating_point_v<T> && std::is_floating_point_v<decltype(value)> && !std::is_constant_evaluated()) {
		return ::round(value);
	}

	return floor<T>(value + static_cast<decltype(value)>(0.5));
}

[[nodiscard]] FORCEINLINE constexpr auto make_unsigned(const std::unsigned_integral auto value) -> decltype(value) {
	return value;
}

[[nodiscard]] FORCEINLINE constexpr auto count_trailing_zeros(const std::integral auto value) -> u32 {
	return value == 0 ? sizeof(value) * 8 : std::countr_zero(make_unsigned(value));
}

[[nodiscard]] FORCEINLINE constexpr auto count_leading_zeros(const std::integral auto value) -> u32 {
	return value == 0 ? sizeof(value) * 8 : std::countl_zero(make_unsigned(value));
}

[[nodiscard]] FORCEINLINE constexpr auto count_set_bits(const std::integral auto value) -> u32 {
	return std::popcount(make_unsigned(value));
}

[[nodiscard]] FORCEINLINE constexpr auto ceil_log_two(const std::integral auto value) -> u32 {
	static constexpr auto NUM_BITS = sizeof(value) * 8;
	return NUM_BITS - count_leading_zeros(value > 0 ? value - static_cast<decltype(value)>(1) : static_cast<decltype(value)>(1));
}

[[nodiscard]] FORCEINLINE constexpr auto floor_log_two(const std::integral auto value) -> u32 {
	static constexpr auto NUM_BITS = sizeof(value) * 8;
	return (NUM_BITS - 1) - count_leading_zeros(value > 0 ? value : static_cast<decltype(value)>(1));
}

[[nodiscard]] FORCEINLINE constexpr auto round_up_to_power_of_two(const std::integral auto value) -> u32 {
	return 1u << ceil_log_two(value);
}

[[nodiscard]] FORCEINLINE constexpr auto round_down_to_power_of_two(const std::integral auto value) -> u32 {
	return 1u << floor_log_two(value);
}

// The number of bits required to represent this value.
[[nodiscard]] FORCEINLINE constexpr auto num_bits_required(const std::integral auto value) -> u32 {
	return std::bit_width(make_unsigned(value));
}

// Accepts any integral or pointer type for value.
[[nodiscard]] FORCEINLINE constexpr auto align(const auto value, const std::integral auto alignment) requires (std::integral<decltype(value)> || std::is_pointer_v<decltype(value)>) {
	return (make_unsigned(value) + alignment - 1) & ~(alignment - 1);
}

[[nodiscard]] FORCEINLINE constexpr auto is_aligned(const auto value, const auto alignment) -> bool {
	return align(value, alignment) == value;
}

[[nodiscard]] FORCEINLINE constexpr auto hash_combine(u32 a, u32 b) -> u32 {
	u32 c = 0x9e3779b9;
	a += c;

	a -= c; a -= b; a ^= (b>>13);
	c -= b; c -= a; c ^= (a<<8);
	b -= a; b -= c; b ^= (c>>13);
	a -= c; a -= b; a ^= (b>>12);
	c -= b; c -= a; c ^= (a<<16);
	b -= a; b -= c; b ^= (c>>5);
	a -= c; a -= b; a ^= (b>>3);
	c -= b; c -= a; c ^= (a<<10);
	b -= a; b -= c; b ^= (c>>15);

	return b;
}
}
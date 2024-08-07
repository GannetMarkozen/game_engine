#pragma once

#include "defines.hpp"
#include "threading/thread_safe_types.hpp"

template <std::unsigned_integral Word>
struct ConstBitRef {
	constexpr ConstBitRef(const ConstBitRef&) = default;

	FORCEINLINE constexpr ConstBitRef(const Word& value [[clang::lifetimebound]], const Word mask)
		: value{value}, mask{mask} {}

	[[nodiscard]] FORCEINLINE constexpr auto get() const -> bool {
		return value & mask;
	}

	[[nodiscard]] FORCEINLINE constexpr auto operator*() const -> bool {
		return get();
	}

	[[nodiscard]] FORCEINLINE constexpr operator bool() const {
		return get();
	}

	[[nodiscard]] FORCEINLINE constexpr auto operator!() const -> bool {
		return !get();
	}

	[[nodiscard]] FORCEINLINE constexpr auto operator==(const bool other) const -> bool {
		return get() == other;
	}

private:
	const Word& value;
	const Word mask;
};

template <std::unsigned_integral Word>
struct BitRef {
	constexpr BitRef(const BitRef&) = default;

	FORCEINLINE constexpr BitRef(Word& value [[clang::lifetimebound]], const Word mask)
		: value{value}, mask{mask} {}

	[[nodiscard]] FORCEINLINE constexpr auto get() const -> bool {
		return value & mask;
	}

	FORCEINLINE constexpr auto set(const bool new_value) const -> const BitRef& {
		if (new_value) {
			value |= mask;
		} else {
			value &= ~mask;
		}
		return *this;
	}

	FORCEINLINE constexpr auto operator=(const bool new_value) const -> const BitRef& {
		return set(new_value);
	}

	auto atomic_set(const bool new_value, const std::memory_order memory_order = std::memory_order_consume) -> BitRef& {
		Word expected = reinterpret_cast<Atomic<Word>&>(value).load(std::memory_order_relaxed);

		if (new_value) {
			while ((expected & mask) != mask && !reinterpret_cast<Atomic<Word>&>(value).compare_exchange_weak(expected, expected | mask, memory_order, std::memory_order_relaxed)) [[likely]] {
				std::this_thread::yield();
				//expected = reinterpret_cast<Atomic<Word>&>(value).load(std::memory_order_relaxed);
			}
		} else {
			while ((expected & mask) && !reinterpret_cast<Atomic<Word>&>(value).compare_exchange_weak(expected, expected & ~mask, memory_order, std::memory_order_relaxed)) [[likely]] {
				std::this_thread::yield();
				//expected = reinterpret_cast<Atomic<Word>&>(value).load(std::memory_order_relaxed);
			}
		}

		return *this;
	}

	[[nodiscard]] auto atomic_get(const std::memory_order memory_order = std::memory_order_acquire) const -> bool {
		return reinterpret_cast<const Atomic<Word>&>(value).load(memory_order) & mask;
	}

	[[nodiscard]] FORCEINLINE constexpr auto operator*() const -> bool {
		return get();
	}

	[[nodiscard]] FORCEINLINE constexpr operator bool() const {
		return get();
	}

	[[nodiscard]] FORCEINLINE constexpr auto operator!() const -> bool {
		return !get();
	}

	[[nodiscard]] FORCEINLINE constexpr operator ConstBitRef<Word>() const {
		return ConstBitRef<Word>{value, mask};
	}

	[[nodiscard]] FORCEINLINE constexpr auto operator==(const bool other) const -> bool {
		return get() == other;
	}

private:
	Word& value;
	const Word mask;
};

namespace utils {
template <std::unsigned_integral Word>
[[nodiscard]] FORCEINLINE constexpr auto make_bit_ref(Word& value, const Word mask) -> BitRef<Word> {
	return {value, mask};
}

template <std::unsigned_integral Word>
[[nodiscard]] FORCEINLINE constexpr auto make_bit_ref(const Word& value, const Word mask) -> ConstBitRef<Word> {
	return {value, mask};
}
}
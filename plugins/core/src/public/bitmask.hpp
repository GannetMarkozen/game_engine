#pragma once

#include "types.hpp"
#include "math.hpp"
#include "assert.hpp"
#include "utils.hpp"

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

template <usize N>
using SizedUnsignedIntegral = std::conditional_t<
	(N <= 8), u8, std::conditional_t<
	(N <= 16), u16, std::conditional_t<
	(N <= 32), u32, u64>>>;

template <usize N, std::unsigned_integral Word = SizedUnsignedIntegral<N>>
struct BitMask {
	static_assert(N > 0, "N must be greater than 0!");

	template <usize, std::unsigned_integral>
	friend struct BitMask;

	template <typename>
	friend struct std::hash;

	using WordType = Word;
	static constexpr usize WORD_BITS = sizeof(Word) * 8;

	// Number of words.
	static constexpr usize WORD_COUNT = math::divide_and_round_up(N, sizeof(Word) * 8);

	FORCEINLINE constexpr BitMask() {
		if consteval {
			for (usize i = 0; i < WORD_COUNT; ++i) {
				data[i] = 0;
			}
		} else {
			memset(&data[0], 0, WORD_COUNT * sizeof(Word));
		}
	}

	FORCEINLINE constexpr explicit BitMask(NoInit) {}

	constexpr BitMask(const BitMask&) = default;
	constexpr auto operator=(const BitMask&) -> BitMask& = default;

	template <usize OTHER_N>
	[[nodiscard]] FORCEINLINE constexpr auto operator==(const BitMask<OTHER_N, Word>& other) const -> bool {
		static constexpr usize OTHER_WORD_COUNT = std::decay_t<decltype(other)>::WORD_COUNT;
		#pragma unroll
		for (usize i = 0; i < std::min(WORD_COUNT, OTHER_WORD_COUNT); ++i) {
			if (data[i] != other.data[i]) {
				return false;
			}
		}
		if constexpr (OTHER_WORD_COUNT > WORD_COUNT) {
			#pragma once
			for (usize i = WORD_COUNT; i < OTHER_WORD_COUNT; ++i) {
				if (other.data[i] != 0) {
					return false;
				}
			}
		}
		return true;
	}

	[[nodiscard]] FORCEINLINE constexpr auto get_data(this auto&& self) {
		return &self.data[0];
	}

	[[nodiscard]] FORCEINLINE constexpr auto has_any_set_bits() const -> bool {
		#pragma unroll
		for (const auto& word : data) {
			if (word != 0) {
				return true;
			}
		}

		return false;
	}

	[[nodiscard]] FORCEINLINE constexpr operator bool() const {
		return has_any_set_bits();
	}

	[[nodiscard]] FORCEINLINE constexpr auto operator[](this auto&& self, const std::integral auto index) {
		ASSERTF(index >= 0 && index < N, "Index {} out of range {}!", index, N);
		return utils::make_bit_ref<Word>(self.data[index / WORD_BITS], static_cast<Word>(1) << index);
	}

	template <usize OTHER_N>
	FORCEINLINE constexpr auto operator&=(const BitMask<OTHER_N, Word>& other) -> BitMask& {
		static constexpr usize COUNT = math::divide_and_round_up(std::min(N, OTHER_N), WORD_BITS);
		#pragma unroll
		for (usize i = 0; i < COUNT; ++i) {
			data[i] &= other.data[i];
		}
		// If this mask is larger than the other, assume all other bits from the other mask are not set so zero out the remaining words.
		static constexpr usize OTHER_WORD_COUNT = std::decay_t<decltype(other)>::WORD_COUNT;
		if constexpr (WORD_COUNT > OTHER_WORD_COUNT) {
			static constexpr usize EXTRANEOUS_WORD_COUNT = WORD_COUNT - OTHER_WORD_COUNT;
			if consteval {
				#pragma unroll
				for (usize i = COUNT; i < COUNT + EXTRANEOUS_WORD_COUNT; ++i) {
					data[i] = 0;
				}
			} else {
				memset(&data[COUNT], 0, EXTRANEOUS_WORD_COUNT * sizeof(Word));
			}
		}
		return *this;
	}

	template <usize OTHER_N>
	FORCEINLINE constexpr auto operator|=(const BitMask<OTHER_N, Word>& other) -> BitMask& {
		static constexpr usize COUNT = math::divide_and_round_up(std::min(N, OTHER_N), WORD_BITS);
		#pragma unroll
		for (usize i = 0; i < COUNT; ++i) {
			data[i] |= other.data[i];
		}
		if constexpr (OTHER_N > N) {// Potentially set extraneous bits.
			zero_extraneous_bits();
		}
		return *this;
	}

	template <usize OTHER_N>
	FORCEINLINE constexpr auto operator^=(const BitMask<OTHER_N, Word>& other) -> BitMask& {
		static constexpr usize COUNT = math::divide_and_round_up(std::min(N, OTHER_N), WORD_BITS);
		#pragma unroll
		for (usize i = 0; i < COUNT; ++i) {
			data[i] ^= other.data[i];
		}
		if constexpr (OTHER_N > N) {// Potentially set extraneous bits.
			zero_extraneous_bits();
		}
		return *this;
	}

	template <typename Self>
	[[nodiscard]] FORCEINLINE constexpr auto get_word(this Self&& self, const std::integral auto index) -> decltype(auto) {
		ASSERTF(index > 0 && index < WORD_COUNT, "Index {} out of range {}!", index, WORD_COUNT);
		return (std::forward_like<Self>(self.data[index]));
	}

	[[nodiscard]] FORCEINLINE constexpr auto find_first_set_bit() const -> Optional<usize> {
		#pragma unroll
		for (usize i = 0; i < WORD_COUNT; ++i) {
			if (data[i] != 0) {
				return {math::count_trailing_zeros(data[i]) + i * WORD_BITS};
			}
		}

		return NULL_OPTIONAL;
	}

	[[nodiscard]] FORCEINLINE constexpr auto count_set_bits() const -> usize {
		usize count = 0;
		#pragma unroll
		for (usize i = 0; i < WORD_COUNT; ++i) {
			count += math::count_set_bits(data[i]);
		}
		return count;
	}

	[[nodiscard]] constexpr auto find_first_set_bit(const std::integral auto start) const -> Optional<usize> {
		ASSERTF(start > 0 && start < WORD_COUNT, "Start index {} out of range {}!", start, WORD_COUNT);

		const usize start_word_index = start / WORD_BITS;
		const usize start_bit_index = start % WORD_BITS;

		const Word start_mask = data[start_word_index] & std::numeric_limits<Word>::max() << start_bit_index;
		if (start_mask != 0) {
			return {math::count_trailing_zeros(start_mask)};
		}

		for (usize i = start_word_index + 1; i < WORD_COUNT; ++i) {
			if (data[i] != 0) {
				return {math::count_trailing_zeros(data[i]) + i * WORD_BITS};
			}
		}

		return NULL_OPTIONAL;
	}

	FORCEINLINE constexpr auto for_each_set_bit(cpts::Invokable<usize> auto&& fn) const -> void {
		for (usize i = 0; i < WORD_COUNT; ++i) {
			for (Word mask = data[i]; mask; mask &= mask - 1) {
				std::invoke(fn, math::count_trailing_zeros(mask) + i * WORD_BITS);
			}
		}
	}

	[[nodiscard]] FORCEINLINE constexpr auto hash() const -> usize {
		usize hash = std::hash<Word>{}(data[0]);
		#pragma unroll
		for (usize i = 1; i < WORD_COUNT; ++i) {
			hash = math::hash_combine(hash, std::hash<Word>{}(data[i]));
		}
		return hash;
	}

private:
	// No-op if there are no extraneous bits.
	FORCEINLINE constexpr auto zero_extraneous_bits() -> void {
		static constexpr usize EXTRANEOUS_COUNT = (WORD_COUNT * WORD_BITS) - N;
		if constexpr (EXTRANEOUS_COUNT > 0) {
			data[WORD_COUNT - 1] &= std::numeric_limits<Word>::max() >> EXTRANEOUS_COUNT;
		}
	}

	StaticArray<Word, WORD_COUNT> data;
};

template <std::unsigned_integral Word, usize N1, usize N2>
[[nodiscard]] FORCEINLINE constexpr auto operator&(const BitMask<N1, Word>& a, const BitMask<N2, Word>& b) {
	if constexpr (N1 > N2) {
		return auto{a} &= b;
	} else {
		return auto{b} &= a;
	}
}

template <std::unsigned_integral Word, usize N1, usize N2>
[[nodiscard]] FORCEINLINE constexpr auto operator|(const BitMask<N1, Word>& a, const BitMask<N2, Word>& b) {
	if constexpr (N1 > N2) {
		return auto{a} |= b;
	} else {
		return auto{b} |= a;
	}
}

template <std::unsigned_integral Word, usize N1, usize N2>
[[nodiscard]] FORCEINLINE constexpr auto operator^(const BitMask<N1, Word>& a, const BitMask<N2, Word>& b) {
	if constexpr (N1 > N2) {
		return auto{a} ^= b;
	} else {
		return auto{b} ^= a;
	}
}

namespace std {
template <usize N, std::unsigned_integral Word>
struct hash<BitMask<N, Word>> {
	[[nodiscard]] FORCEINLINE constexpr auto operator()(const BitMask<N, Word>& value) const -> usize {
		return value.hash();
	}
};
}
#pragma once

#include "types.hpp"
#include "math.hpp"
#include "bitref.hpp"
#include "assert.hpp"

template <std::unsigned_integral Word = u64, typename Allocator = std::allocator<Word>>
struct BitMask {
	static constexpr usize BITS_PER_WORD = sizeof(Word) * 8;

	constexpr BitMask(const BitMask&) = default;
	constexpr auto operator=(const BitMask&) -> BitMask& = default;

	constexpr BitMask()
		: count{0} {}

	constexpr BitMask(BitMask&& other) noexcept
		: words{std::move(other.words)} {
		if (this == &other) [[unlikely]] {
			return;
		}

		count = other.count;
		other.count = 0;
	}

	constexpr auto operator=(BitMask&& other) noexcept -> BitMask& {
		if (this == &other) {
			return *this;
		}

		words = std::move(other.words);

		count = other.count;
		other.count = 0;
	}

	[[nodiscard]] FORCEINLINE constexpr auto get_count() const -> usize {
		return count;
	}

	[[nodiscard]] FORCEINLINE constexpr auto is_empty() const -> bool {
		return count == 0;
	}

	[[nodiscard]] FORCEINLINE constexpr auto operator[](this auto&& self, const std::integral auto index) {
		ASSERTF(index >= 0 && index < self.count, "Attempted to access index {} of BitArray sized {}!", index, self.count);
		return utils::make_bit_ref(self.words[index / BITS_PER_WORD], static_cast<Word>(1) << index % BITS_PER_WORD);
	}

	// Reserves enough space to fit this index and sets the corresponding bit to true. Never asserts (unlike operator[]).
	FORCEINLINE constexpr auto set(const std::integral auto index) -> BitMask& {
		resize_to_fit(index + 1);
		(*this)[index] = true;
		return *this;
	}

	// Resizes to fit atleast this number of elements.
	constexpr auto resize_to_fit(const std::integral auto min_count) -> bool {
		if (count >= min_count) {
			return false;
		}

		const usize new_num_words = math::divide_and_round_up(min_count, BITS_PER_WORD);
		if (new_num_words > math::divide_and_round_up(count, BITS_PER_WORD)) [[unlikely]] {
			words.resize(new_num_words);// Should default to 0s.
		}

		count = min_count;

		return true;
	}

	constexpr auto for_each_set_bit(cpts::Invokable<usize> auto&& fn) const -> void {
		for (usize i = 0; i < words.size(); ++i) {
			for (Word word = words[i]; word; word &= word - 1) {
				std::invoke(fn, math::count_trailing_zeros(word) + i * BITS_PER_WORD);
			}
		}
	}

	[[nodiscard]] constexpr auto count_set_bits() const -> usize {
		usize out = 0;
		for (const Word& RESTRICT word : words) {
			out += math::count_set_bits(word);
		}
		return out;
	}

	constexpr auto operator&=(const BitMask& other) -> BitMask& {
		for (usize i = 0; i < std::min(words.size(), other.words.size()); ++i) {
			words[i] &= other.words[i];
		}

		if (words.size() > other.words.size()) {
			for (usize i = other.words.size(); i < words.size(); ++i) {
				words[i] = 0;
			}
		}

		return *this;
	}

	constexpr auto operator|=(const BitMask& other) -> BitMask& {
		resize_to_fit(other.count);

		for (usize i = 0; i < std::min(words.size(), other.words.size()); ++i) {
			words[i] |= other.words[i];
		}

		return *this;
	}

	constexpr auto operator^=(const BitMask& other) -> BitMask& {
		resize_to_fit(other.count);

		for (usize i = 0; i < std::min(words.size(), other.words.size()); ++i) {
			words[i] ^= other.words[i];
		}

		return *this;
	}

	[[nodiscard]] FORCEINLINE constexpr friend auto operator&(BitMask a, const BitMask& b) -> BitMask {
		return a &= b;
	}

	[[nodiscard]] FORCEINLINE constexpr friend auto operator|(BitMask a, const BitMask& b) -> BitMask {
		return a |= b;
	}

	[[nodiscard]] FORCEINLINE constexpr friend auto operator^(BitMask a, const BitMask& b) -> BitMask {
		return a ^= b;
	}

	[[nodiscard]] FORCEINLINE constexpr friend auto operator~(BitMask value) -> BitMask {
		for (Word& word : value.words) {
			word = ~word;
		}

		return value;
	}

private:
	Array<Word, Allocator> words;
	usize count;
};
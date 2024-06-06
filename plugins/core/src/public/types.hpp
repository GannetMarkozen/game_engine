#pragma once

#include <variant>
#include <bitset>
#include "math.hpp"
#include "assert.hpp"

template<typename... Ts>
class Variant : public std::variant<Ts...> {
public:
	using std::variant<Ts...>::variant;

	template<typename T>
	static consteval fn contains() -> bool {
		return (std::is_same_v<T, Ts> || ...);
	}

	template<typename T>
	static consteval fn index_of() -> usize {
		static_assert(contains<T>(), "Variant does not contain this type!");
		usize index = 0;
		const bool found = ((std::is_same_v<T, Ts> ? true : [&] { ++index; return false; }()) || ...);
		return index;
	}

	template<usize INDEX>
	using TypeAt = std::tuple_element_t<INDEX, std::tuple<Ts...>>;

	template<typename T>
	[[nodiscard]]
	FORCEINLINE constexpr fn get() -> T& {
		ASSERTF(index() == index_of<T>(), "Attempted to get index {} when the variant contains index {}!", index_of<T>(), index());
		return std::get<T>(*this);
	}

	template<typename T>
	[[nodiscard]]
	FORCEINLINE constexpr fn get() const -> const T& {
		ASSERTF(index() == index_of<T>(), "Attempted to get index {} when the variant contains index {}!", index_of<T>(), index());
		return std::get<T>(*this);
	}

	template<Invokable Func, typename... Args>
	FORCEINLINE constexpr fn match(Func&& func, Args&&... args) const -> decltype(std::forward<Func>(func).template operator()<TypeAt<0>>(std::forward<Args>(args)...)) {
		using Return = decltype(std::forward<Func>(func).template operator()<TypeAt<0>>(std::forward<Args>(args)...));

		static constexpr Return(*table[])(Func&&, Args&&...) {
			[](Func&& func, Args&&... args) -> Return {
				if constexpr (std::is_same_v<Return, void>) {
					std::forward<Func>(func).template operator()<Ts>(std::forward<Args>(args)...);
				} else {
					return std::forward<Func>(func).template operator()<Ts>(std::forward<Args>(args)...);
				}
			}...
		};
		static_assert(std::size(table) == sizeof...(Ts));

		if constexpr (std::is_same_v<Return, void>) {
			table[index()](std::forward<Func>(func), std::forward<Args>(args)...);
		} else {
			return table[index()](std::forward<Func>(func), std::forward<Args>(args)...);
		}
	}
};

template<usize BITS>
using BitSet = std::bitset<BITS>;

// @TODO: Make an inline allocator for this.
template<typename Word = u32, typename Allocator = std::allocator<Word>>
class BitArray {
public:
	static constexpr usize NUM_BYTES_PER_WORD = sizeof(Word);
	static constexpr usize NUM_BITS_PER_WORD = NUM_BYTES_PER_WORD * 8;

	struct ConstBitReference {
		constexpr explicit ConstBitReference(const Word mask, Word& value)
			: mask{mask}, value{value} {}

		constexpr ConstBitReference(const ConstBitReference& other)
			: ConstBitReference{other.mask, other.value} {}

		[[nodiscard]]
		FORCEINLINE constexpr operator bool() const {
			return mask & value;
		}

	protected:
		Word mask;
		Word& value;
	};

	struct BitReference : public ConstBitReference {
		using ConstBitReference::ConstBitReference;

		FORCEINLINE constexpr fn operator=(const bool in_value) -> BitReference& {
		if (in_value) {
			this->value |= this->mask;
		} else {
			this->value &= ~this->mask;
		}
		return *this;
	}

		FORCEINLINE constexpr fn operator=(const ConstBitReference& other) -> BitReference& {
			return *this = static_cast<bool>(other);
		}
	};

	[[nodiscard]]
	fn operator[](const std::integral auto index) -> BitReference {
		ASSERTF(index < num_bits, "Attempted to access index {} when there are {} bits allocated!", index, num_bits);
		return BitReference{index % NUM_BITS_PER_WORD, array[index / NUM_BITS_PER_WORD]};
	}

	[[nodiscard]]
	fn operator[](const std::integral auto index) const -> ConstBitReference {
		return (*const_cast<BitArray*>(this))[index];
	}

	[[nodiscard]]
	fn operator==(const BitArray& other) const -> bool {
		if (num_bits != other.num_bits) return false;
		for (usize i = 0; i < array.size(); ++i) {
			if (array[i] != other.array[i]) {
				return false;
			}
		}
		return true;
	}

	fn operator|=(const BitArray& other) -> BitArray& {
		set_min_size_bits(other.num_bits);
		for (usize i = 0; i < other.array.size(); ++i) {
			array[i] |= other.array[i];
		}
		return *this;
	}

	fn operator&=(const BitArray& other) -> BitArray& {
		set_min_size_bits(other.num_bits);
		for (usize i = 0; i < other.array.size(); ++i) {
			array[i] &= other.array[i];
		}
		return *this;
	}

	[[nodiscard]]
	fn operator|(const BitArray& other) const -> BitArray {
		BitArray copy = *this;
		return copy |= other;
	}

	[[nodiscard]]
	fn operator&(const BitArray& other) const -> BitArray {
		BitArray copy = *this;
		return copy &= other;
	}

	template<std::integral Int>
	fn for_each_set_bit(Invokable<Int> auto&& func) -> void {
		for (usize i = 0; i < array.size(); ++i) {
			for (Word mask = array[i]; mask; mask &= mask - 1) {
				std::invoke(func, math::count_trailing_zeros(mask) + i * NUM_BITS_PER_WORD);
			}
		}
	}

	[[nodiscard]]
	fn has_all_zeroes() const -> bool {
		for (const auto word : array) {
			if (word != 0) {
				return false;
			}
		}
		return true;
	}

	[[nodiscard]]
	fn has_all_set_bits(const BitArray& other) const -> bool {
		const auto count = std::min(array.size(), other.array.size());
		for (usize i = 0; i < count; ++i) {
			if ((array[i] & other.array[i]) != array[i]) {
				return false;
			}
		}
		return true;
	}

	[[nodiscard]]
	fn find_first_set_bit() const -> Optional<usize> {
		for (usize i = 0; i < array.size(); ++i) {
			if (array[i] != 0) {
				return {math::count_trailing_zeros(array[i]) + i * NUM_BITS_PER_WORD};
			}
		}
		return {};
	}

	// If there are less words, resizes to fit that many words. Fills with zeros.
	fn set_min_size_words(const usize num_words) -> bool {
		if (array.size() >= num_words) {
			return false;
		}

		const usize add_num_words = num_words - array.size();
		array.reserve(num_words);
		for (usize i = 0; i < add_num_words; ++i) {
			array.push_back(0);
		}

		num_bits = num_words * NUM_BITS_PER_WORD;
		return true;
	}

	// If there are less bits, resizes to fit that many bits. Fills with zeros.
	fn set_min_size_bits(const usize in_num_bits) -> bool {
		if (num_bits >= in_num_bits) {
			return false;
		}

		const auto new_num_words = math::divide_and_round_up(in_num_bits, NUM_BITS_PER_WORD);
		if (new_num_words > array.size()) [[unlikely]] {
			array.reserve(new_num_words);

			const auto add_num_words = new_num_words - array.size();
			for (usize i = 0; i < add_num_words; ++i) {
				array.push_back(0);
			}
		}

		num_bits = in_num_bits;

		return true;
	}

	// Inserts a bit at the given index. If the index is out of range the array will be resized with zeros to fit.
	fn insert(const usize index, const bool value) -> void {
		set_min_size_bits(index);
		(*this)[index] = value;
	}

	fn is_empty() -> bool {
		return num_bits == 0;
	}

	fn clear() -> void {
		array.clear();
		num_bits = 0;
	}

	fn last() -> BitReference {
		ASSERT(!is_empty());
		return (*this)[num_bits - 1];
	}

	fn last() const -> ConstBitReference {
		ASSERT(!is_empty());
		return (*this)[num_bits - 1];
	}

	fn push(const bool value) -> void {
		const u32 old_num_bytes = math::divide_and_round_up(num_bits, NUM_BITS_PER_WORD);
		const u32 new_num_bytes = math::divide_and_round_up(++num_bits, NUM_BITS_PER_WORD);

		if (new_num_bytes != old_num_bytes) [[unlikely]] {
			array.push_back(0);
		}

		(*this)[num_bits - 1] = value;
	}

	fn pop() -> Optional<bool> {
		if (is_empty()) return {};

		const bool value = last();
		--num_bits;

		if (num_bits % NUM_BITS_PER_WORD == 0) [[unlikely]] {
			array.pop_back();
		}

		return {value};
	}

	[[nodiscard]]
	FORCEINLINE fn size() const -> usize {
		return num_bits;
	}

	Array<Word, Allocator> array;

private:
	usize num_bits = 0;
};
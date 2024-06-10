#pragma once

#include <variant>
#include <bitset>
#include "math.hpp"
#include "assert.hpp"

template<typename T>
consteval fn get_type_name() -> StringView {
	const StringView name = __PRETTY_FUNCTION__;
#if defined(__clang__) || defined(__GNUC__)
    // Extract between "T = " and "]"
    const auto start = name.find('=') + 2;
    const auto end = name.rfind(']');
    return name.substr(start, end - start);
#elif defined(_MSC_VER)
    // Extract between "type_name<" and ">(void)"
    const auto start = name.find('<') + 1;
    const auto end = name.rfind('>');
    return name.substr(start, end - start);
#else
    return name;
#endif
}

namespace utils {
template<typename T, typename... Ts>
[[nodiscard]]
consteval fn contains_type() -> bool {
	return (std::is_same_v<T, Ts> || ...);
}

template<typename T, typename... Ts>
requires (contains_type<T, Ts...>())
[[nodiscard]]
consteval fn index_of_type() -> usize {
	usize index = 0;
	((std::is_same_v<T, Ts> ? true : [&] { ++index; return false; }()) || ...);
	return index;
}
}

template<typename... Ts>
class Variant : public std::variant<Ts...> {
public:
	using std::variant<Ts...>::variant;

	template<typename T>
	[[nodiscard]]
	static consteval fn contains() -> bool {
		return (std::is_same_v<T, Ts> || ...);
	}

	template<typename T>
	requires (contains<T>())
	[[nodiscard]]
	static consteval fn index_of() -> usize {
		usize index = 0;
		((std::is_same_v<T, Ts> ? true : [&] { ++index; return false; }()) || ...);
		return index;
	}

	template<usize INDEX>
	using TypeAt = std::tuple_element_t<INDEX, std::tuple<Ts...>>;

private:
	// Silences weird warnings from calling .index() within this class.
	[[nodiscard]]
	FORCEINLINE static fn get_index_hack(const std::variant<Ts...>& variant) {
		return variant.index();
	}

	template<typename T, typename... Args>
	FORCEINLINE static fn emplace_hack(std::variant<Ts...>& variant, Args&&... args) -> void {
		variant.template emplace<T>(std::forward<Args>(args)...);
	}

public:

	template<typename T>
	requires (contains<T>())
	[[nodiscard]]
	FORCEINLINE constexpr fn is() const -> bool {
		return get_index_hack(*this) == index_of<T>();
	}

	template<typename T>
	requires (contains<T>())
	[[nodiscard]]
	FORCEINLINE constexpr fn get() -> T& {
		ASSERTF(is<T>(), "Attempted to get {} when variant is {}!", get_type_name<T>(), match([]<typename ActualType>() { return get_type_name<ActualType>(); }));
		return std::get<T>(*this);
	}

	template<typename T>
	requires (contains<T>())
	[[nodiscard]]
	FORCEINLINE constexpr fn get() const -> const T& {
		ASSERTF(is<T>(), "Attempted to get {} when variant is {}!", get_type_name<T>(), match([]<typename ActualType>() { return get_type_name<ActualType>(); }));
		return std::get<T>(*this);
	}

	template<typename T>
	requires (contains<T>())
	[[nodiscard]]
	FORCEINLINE constexpr fn try_get() -> T* {
		return is<T>() ? &get<T>() : nullptr;
	}

	template<typename T>
	requires (contains<T>())
	[[nodiscard]]
	FORCEINLINE constexpr fn try_get() const -> const T* {
		return is<T>() ? &get<T>() : nullptr;
	}

	template<typename Func, typename... Args>
	FORCEINLINE static constexpr fn match_index(const std::integral auto index, Func&& func, Args&&... args) -> decltype(std::forward<Func>(func).template operator()<TypeAt<0>>(std::forward<Args>(args)...)) {
		ASSERTF(index >= 0 && index < sizeof...(Ts), "Attempted to index into type at index {} when num types is {}!", index, sizeof...(Ts));

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
			table[index](std::forward<Func>(func), std::forward<Args>(args)...);
		} else {
			return table[index](std::forward<Func>(func), std::forward<Args>(args)...);
		}
	}

	template<typename Func, typename... Args>
	FORCEINLINE constexpr fn match(Func&& func, Args&&... args) const -> decltype(match_index(0, std::forward<Func>(func), std::forward<Args>(args)...)) {
		return match_index(get_index_hack(*this), std::forward<Func>(func), std::forward<Args>(args)...);
	}

	template<typename T>
	requires (contains<std::decay_t<T>>())
	FORCEINLINE constexpr fn set(T&& value) -> T& {
		emplace_hack<std::decay_t<T>>(*this, std::forward<T>(value));
		return get<std::decay_t<T>>();
	}
};

template<usize BITS>
using BitSet = std::bitset<BITS>;

// @TODO: Make an inline allocator for this otherwise it will be super slow.
template<std::unsigned_integral Word = u32, typename Allocator = std::allocator<Word>>
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
		return BitReference{static_cast<Word>(1) << (index % NUM_BITS_PER_WORD), array[index / NUM_BITS_PER_WORD]};
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

	[[nodiscard]]
	fn operator|(const BitArray& other) && -> BitArray {
		BitArray copy = std::move(*this);
		return copy |= other;
	}

	[[nodiscard]]
	fn operator&(const BitArray& other) && -> BitArray {
		BitArray copy = std::move(*this);
		return copy &= other;
	}

	fn for_each_set_bit(Invokable<u32> auto&& func) const -> void {
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
	fn has_all_matching_set_bits(const BitArray& other) const -> bool {
		const auto count = std::min(array.size(), other.array.size());
		for (usize i = 0; i < count; ++i) {
			if ((array[i] & other.array[i]) != array[i]) {
				return false;
			}
		}
		return true;
	}

	[[nodiscard]]
	fn has_any_matching_set_bits(const BitArray& other) const -> bool {
		const auto count = std::min(array.size(), other.array.size());
		for (usize i = 0; i < count; ++i) {
			if (array[i] & other.array[i]) {
				return true;
			}
		}
		return false;
	}

	[[nodiscard]]
	fn find_first_set_bit(const usize search_start = 0) const -> Optional<usize> {
		for (usize i = search_start; i < array.size(); ++i) {
			if (array[i] != 0) {
				return {math::count_trailing_zeros(array[i]) + i * NUM_BITS_PER_WORD};
			}
		}
		return {};
	}

	[[nodiscard]]
	fn find_first_unset_bit(const usize search_start = 0) const -> Optional<usize> {
		for (usize i = search_start; i < array.size(); ++i) {
			const auto value = ~array[i];
			if (value != 0) {
				return {math::count_trailing_zeros(value) + i * NUM_BITS_PER_WORD};
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
		set_min_size_bits(index + 1);
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

	[[nodiscard]]
	FORCEINLINE fn is_valid_index(const std::integral auto index) const -> bool {
		return index < num_bits;
	}

	[[nodiscard]]
	FORCEINLINE fn count_set_bits() const -> usize {
		usize count = 0;
		for (const auto& word : array) {
			count += math::count_set_bits(word);
		}
		return count;
	}

	Array<Word, Allocator> array;

private:
	usize num_bits = 0;
};

// Subclass this to make an integer type that can't be coerced into other IntAlias types.
template<std::integral T>
struct IntAlias {
	constexpr IntAlias() : value{0} {}
	explicit IntAlias(NoInit) {}
	constexpr explicit IntAlias(const std::integral auto in_value) : value{in_value} {}
	constexpr IntAlias(const IntAlias&) = default;
	constexpr IntAlias(IntAlias&&) noexcept = default;
	constexpr fn operator=(const IntAlias&) -> IntAlias& = default;
	constexpr fn operator=(IntAlias&&) noexcept -> IntAlias& = default;

	[[nodiscard]]
	FORCEINLINE constexpr fn operator==(const std::integral auto other) const { return value == other; }

	template<std::integral Other>
	[[nodiscard]]
	FORCEINLINE constexpr fn operator==(const IntAlias<Other>& other) const { return value == other.value; }

	[[nodiscard]]
	FORCEINLINE constexpr fn operator!=(const std::integral auto other) const { return value != other; }

	template<std::integral Other>
	[[nodiscard]]
	FORCEINLINE constexpr fn operator!=(const IntAlias<Other>& other) const { return value != other.value; }

	[[nodiscard]]
	FORCEINLINE constexpr fn operator<=>(const std::integral auto other) const { return value <=> other; }

	template<std::integral Other>
	[[nodiscard]]
	FORCEINLINE constexpr fn operator<=>(const IntAlias<Other>& other) const { return value <=> other.value; }

	template<typename Other>
	requires std::is_arithmetic_v<T>
	[[nodiscard]]
	FORCEINLINE constexpr operator Other() const { return static_cast<Other>(value); }

	[[nodiscard]] FORCEINLINE constexpr operator T&() { return value; }
	[[nodiscard]] FORCEINLINE constexpr operator const T&() const { return value; }

	FORCEINLINE constexpr fn set_value(const T new_value) -> void { value = new_value; }
	[[nodiscard]] FORCEINLINE constexpr fn get_value() const -> T { return value; }

private:
	T value;
};

namespace std {
template<std::integral T>
struct hash<IntAlias<T>> {
	[[nodiscard]]
	fn operator()(const IntAlias<T> value) { return std::hash<T>{}(value.get_value()); }
};
}

template<typename T>
struct SparseArray {
private:
	struct Uninitialized {
		alignas(T) u8 buffer[sizeof(T)];
	};

public:
	~SparseArray() {
		initialized_mask.for_each_set_bit([&](const u32 index) {
			(*this)[index].~T();
		});
	}

	fn set_min_size(const usize count) -> bool {
		if (count <= size()) {
			return false;
		}

		array.resize(count);
	}

	// Inserts an element at the given index. Resizes the array to fit if required.
	template<typename... Args>
	fn insert(const usize index, Args&&... args) -> T& {
		set_min_size(index + 1);

		if (is_initialized(index)) {
			(*this)[index].~T();
		}

		return *new(&(*this)[index]) T{std::forward<Args>(args)...};
	}

	[[nodiscard]]
	FORCEINLINE constexpr fn operator[](const std::integral auto index) -> T& {
		ASSERT(is_initialized(index));
		return *reinterpret_cast<T*>(array[index].buffer);
	}

	[[nodiscard]]
	FORCEINLINE constexpr fn operator[](const std::integral auto index) const -> const T& {
		ASSERT(is_initialized(index));
		return *reinterpret_cast<const T*>(array[index].buffer);
	}

	[[nodiscard]]
	FORCEINLINE constexpr fn size() const -> usize {
		ASSERT(array.size() == initialized_mask.size());
		return array.size();
	}

	[[nodiscard]]
	FORCEINLINE constexpr fn is_initialized(const std::integral auto index) const -> bool {
		return is_valid_index() && initialized_mask[index];
	}

	[[nodiscard]]
	FORCEINLINE constexpr fn is_valid_index(const std::integral auto index) const -> bool {
		return index >= 0 && index < size();
	}

	[[nodiscard]]
	FORCEINLINE constexpr fn get_initialized_mask() const -> const BitArray<>& {
		return initialized_mask;
	}

private:
	Array<Uninitialized> array;
	BitArray<> initialized_mask;
};

template<typename>
struct FnRef;

// References a function object. Does not do any allocations unlike Fn.
template<typename Return, typename... Args>
struct FnRef<Return(Args...)> {
	template<InvokableReturns<Return, Args...> T>
	FORCEINLINE constexpr FnRef(T&& in_func [[clang::lifetimebound]])
		:
		data{(void*)&in_func},
		func{[](void* data, Args&&... args) -> Return {
			if constexpr (std::is_same_v<Return, void>) {
				std::invoke(*static_cast<std::decay_t<T>*>(data), std::forward<Args>(args)...);
			} else {
				return std::invoke(*static_cast<std::decay_t<T>*>(data), std::forward<Args>(args)...);
			}
		}} {}

	FORCEINLINE constexpr fn operator()(Args... args) const -> Return {
		if constexpr (std::is_same_v<Return, void>) {
			func(data, std::forward<Args>(args)...);
		} else {
			return func(data, std::forward<Args>(args)...);
		}
	}

private:
	void* const data;
	Return(* const func)(void*, Args&&...);
};
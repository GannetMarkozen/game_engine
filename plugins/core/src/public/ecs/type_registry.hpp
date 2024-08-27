#pragma once

#include "types.hpp"
#include "rtti.hpp"
#include "static_bitmask.hpp"
#include "bitmask.hpp"
#include "utils.hpp"
#include "concepts.hpp"

namespace ecs {
// @TODO: Use lazy-initialization instead of global-initialization to avoid some weird edge-cases.
template <cpts::IntAlias IdType>
struct TypeRegistry {
	EXPORT_API inline static Array<const rtti::TypeInfo*> TYPE_INFOS;

private:
	template <typename T>
	struct TypeInstantiator {
		EXPORT_API inline static const IdType ID = [] {
			const usize index = TYPE_INFOS.size();
			ASSERTF(index < IdType::max(), "TypeRegistry for type {} has exceeded {} registered types!", utils::get_type_name<T>(), IdType::max().get_value());
			TYPE_INFOS.push_back(&rtti::get_type_info<T>());
			return IdType{static_cast<u16>(index)};
		}();
	};

public:
	template <typename T>
	[[nodiscard]] FORCEINLINE static auto get_id() -> IdType {
		return TypeInstantiator<T>::ID;
	}

	[[nodiscard]] FORCEINLINE static auto get_type_info(const IdType id) -> const rtti::TypeInfo& {
		ASSERTF(id >= 0 && id < TYPE_INFOS.size(), "Attempted to get type_info at index {} when num registered types is {}!", id.get_value(), TYPE_INFOS.size());
		return *TYPE_INFOS[id.get_value()];
	}

	[[nodiscard]] FORCEINLINE static auto get_num_registered_types() -> usize {
		return TYPE_INFOS.size();
	}
};

template <cpts::IntAlias IdType, usize N, std::unsigned_integral Word = SizedUnsignedIntegral<N>>
struct StaticTypeMask {
	using TypeRegistry = TypeRegistry<IdType>;

	constexpr StaticTypeMask() = default;
	constexpr StaticTypeMask(const StaticTypeMask&) = default;
	constexpr auto operator=(const StaticTypeMask&) -> StaticTypeMask& = default;

	constexpr StaticTypeMask(const StaticBitMask<N, Word>& other)
		: mask{other} {}

	template <typename... Ts>
	[[nodiscard]] FORCEINLINE static auto make() -> StaticTypeMask {
		StaticTypeMask out;
		(out.add<Ts>(), ...);
		return out;
	}

	[[nodiscard]] FORCEINLINE static auto make(::cpts::Range<IdType> auto range) -> StaticTypeMask {
		StaticTypeMask out;
		for (const IdType id : range) {
			out.add(id);
		}
		return out;
	}

	FORCEINLINE constexpr auto add(const IdType id) -> StaticTypeMask& {
		mask[id.get_value()] = true;
		return *this;
	}

	FORCEINLINE constexpr auto remove(const IdType id) -> StaticTypeMask& {
		mask[id.get_value()] = false;
		return *this;
	}

	[[nodiscard]] FORCEINLINE constexpr auto has(const IdType id) const -> bool {
		return mask[id.get_value()];
	}

	template <typename T>
	FORCEINLINE auto add() -> StaticTypeMask& {
		return add(TypeRegistry::template get_id<T>());
	}

	template <typename T>
	FORCEINLINE auto remove() -> StaticTypeMask& {
		return remove(TypeRegistry::template get_id<T>());
	}

	template <typename T>
	FORCEINLINE auto has() const -> bool {
		return has(TypeRegistry::template get_id<T>());
	}

	FORCEINLINE constexpr auto for_each(::cpts::Invokable<IdType> auto&& fn) const -> void {
		mask.for_each_set_bit([&](const usize i) {
			std::invoke(fn, IdType{i});
		}, math::divide_and_round_up(TypeRegistry::get_num_registered_types(), decltype(mask)::WORD_BITS));
	}

	FORCEINLINE constexpr auto for_each_with_break(::cpts::InvokableReturns<bool, IdType> auto&& fn) const -> bool {
		return mask.for_each_set_bit_with_break([&](const usize i) {
			return std::invoke(fn, IdType{i});
		}, math::divide_and_round_up(TypeRegistry::get_num_registered_types(), decltype(mask)::WORD_BITS));
	}

	FORCEINLINE constexpr auto negate() -> StaticTypeMask& {
		mask.negate();
		zero_extraneous_bits();
		return *this;
	}

	[[nodiscard]] FORCEINLINE constexpr auto operator==(const StaticTypeMask& other) const -> bool {
		return mask == other.mask;
	}

	FORCEINLINE constexpr auto operator&=(const StaticTypeMask& other) -> StaticTypeMask& {
		mask &= other.mask;
		return *this;
	}

	FORCEINLINE constexpr auto operator|=(const StaticTypeMask& other) -> StaticTypeMask& {
		mask |= other.mask;
		return *this;
	}

	FORCEINLINE constexpr auto operator^=(const StaticTypeMask& other) -> StaticTypeMask& {
		mask ^= other.mask;
		zero_extraneous_bits();
		return *this;
	}

	[[nodiscard]] FORCEINLINE constexpr operator bool() const {
		return mask.has_any_set_bits();
	}

	[[nodiscard]] FORCEINLINE constexpr auto operator!() const -> bool {
		return !mask.has_any_set_bits();
	}

	[[nodiscard]] FORCEINLINE friend constexpr auto operator&(StaticTypeMask a, const StaticTypeMask& b) -> StaticTypeMask {
		return a &= b;
	}

	[[nodiscard]] FORCEINLINE friend constexpr auto operator|(StaticTypeMask a, const StaticTypeMask& b) -> StaticTypeMask {
		return a |= b;
	}

	[[nodiscard]] FORCEINLINE friend constexpr auto operator^(StaticTypeMask a, const StaticTypeMask& b) -> StaticTypeMask {
		return a ^= b;
	}

	[[nodiscard]] FORCEINLINE friend constexpr auto operator~(StaticTypeMask value) -> StaticTypeMask {
		return value.negate();
	}

	constexpr auto zero_extraneous_bits() -> void {
		ASSERTF(N >= TypeRegistry::get_num_registered_types(), "BitMask has {} bits while TypeRegistry has {} registered types!",
			N, TypeRegistry::get_num_registered_types());

		static constexpr usize WORD_BITS = decltype(mask)::WORD_BITS;
		static constexpr usize WORD_COUNT = decltype(mask)::WORD_COUNT;
		using WordType = typename decltype(mask)::WordType;

		const usize num_extraneous_bits = N - TypeRegistry::get_num_registered_types();
		const usize num_extraneous_words = WORD_COUNT - math::divide_and_round_up(N - num_extraneous_bits, WORD_BITS);
		const usize start_word_index = WORD_COUNT == num_extraneous_words ? 0 : WORD_COUNT - num_extraneous_words - 1;

		mask.get_word(start_word_index) &= std::numeric_limits<WordType>::max() >> (((num_extraneous_bits + WORD_BITS - 1) % WORD_BITS) + 1);
		for (usize i = start_word_index + 1; i < WORD_COUNT; ++i) {
			mask.get_word(i) = 0;
		}
	}

	StaticBitMask<N, Word> mask;
};

template <cpts::IntAlias IdType, std::unsigned_integral Word = u64, typename Allocator = std::allocator<Word>>
struct TypeMask {
	using TypeRegistry = TypeRegistry<IdType>;

	template <typename... Ts>
	[[nodiscard]] FORCEINLINE static auto make() -> TypeMask {
		TypeMask out;
		(out.add<Ts>(), ...);
		return out;
	}

	FORCEINLINE constexpr auto add(const IdType id) -> TypeMask& {
		mask.set(id.get_value());
		return *this;
	}

	[[nodiscard]] FORCEINLINE constexpr auto has(const IdType id) const -> bool {
		return mask[id.get_value()];
	}

	template <typename T>
	FORCEINLINE auto add() -> TypeMask& {
		return add(TypeRegistry::template get_id<T>());
	}

	template <typename T>
	FORCEINLINE auto has() const -> bool {
		return has(TypeRegistry::template get_id<T>());
	}

	FORCEINLINE constexpr auto for_each(cpts::Invokable<IdType> auto&& fn) const -> void {
		mask.for_each_set_bit([&](const usize i) {
			std::invoke(fn, IdType{static_cast<u16>(i)});
		});
	}

	FORCEINLINE constexpr auto flip_bits() -> TypeMask& {
		mask.resize_to_fit(TypeRegistry::get_num_registered_types());
		mask.flip_bits();

		return *this;
	}

	[[nodiscard]] FORCEINLINE constexpr auto operator==(const TypeMask& other) const -> bool {
		return mask == other.mask;
	}

	FORCEINLINE constexpr auto operator&=(const TypeMask& other) -> TypeMask& {
		mask &= other.mask;
		return *this;
	}

	FORCEINLINE constexpr auto operator|=(const TypeMask& other) -> TypeMask& {
		mask |= other.mask;
		return *this;
	}

	FORCEINLINE constexpr auto operator^=(const TypeMask& other) -> TypeMask& {
		mask ^= other.mask;
		return *this;
	}

	[[nodiscard]] FORCEINLINE friend constexpr auto operator&(TypeMask a, const TypeMask& b) -> TypeMask {
		return a &= b;
	}

	[[nodiscard]] FORCEINLINE friend constexpr auto operator|(TypeMask a, const TypeMask& b) -> TypeMask {
		return a |= b;
	}

	[[nodiscard]] FORCEINLINE friend constexpr auto operator^(TypeMask a, const TypeMask& b) -> TypeMask {
		return a ^= b;
	}

	[[nodiscard]] FORCEINLINE friend constexpr auto operator~(TypeMask value) -> TypeMask {
		return value.flip_bits();
	}

	BitMask<Word, Allocator> mask;
};

template <cpts::IntAlias T>
[[nodiscard]] FORCEINLINE auto get_type_info(const T id) -> const rtti::TypeInfo& {
	return TypeRegistry<T>::get_type_info(id);
}
}

namespace std {
template <cpts::IntAlias T, usize N, std::unsigned_integral Word>
struct hash<ecs::StaticTypeMask<T, N, Word>> {
	[[nodiscard]] FORCEINLINE constexpr auto operator()(const ecs::StaticTypeMask<T, N, Word>& value) const -> usize {
		return value.mask.hash();
	}
};
}

namespace ecs {
// @TODO: Should take an Allocator argument.
// Automatically allocates and default-constructs on instantiation.
template <cpts::IntAlias IdType, typename T>
struct TypeArray {
	using TypeRegistry = TypeRegistry<IdType>;

	constexpr TypeArray()
		: data{new T[TypeRegistry::get_num_registered_types()]} {}

	constexpr TypeArray(NoInit) {}

	constexpr TypeArray(const TypeArray& other) {
		if (other.is_empty()) {
			data = null;
			return;
		}

		data = new std::aligned_storage<sizeof(T), alignof(T)>[TypeRegistry::get_num_registered_types()];
		if constexpr (std::is_trivially_copy_constructible_v<T>) {
			memcpy(data, other.data, TypeRegistry::get_num_registered_types() * sizeof(T));
		} else {
			for (usize i = 0; i < TypeRegistry::get_num_registered_types(); ++i) {
				std::construct_at(&data[i], other.data[i]);
			}
		}
	}

	constexpr TypeArray(TypeArray&& other) noexcept {
		if (this == &other) [[unlikely]] {
			return;
		}

		data = other.data;
		other.data = null;
	}

	FORCEINLINE constexpr auto operator=(const TypeArray& other) -> TypeArray& {
		if (other.is_empty()) {
			reset();
			return *this;
		} else if (is_empty()) {
			data = new T[TypeRegistry::get_num_registered_types()];
		}

		if constexpr (std::is_trivially_copy_constructible_v<T>) {
			memcpy(data, other.data, TypeRegistry::get_num_registered_types() * sizeof(T));
		} else {
			for (usize i = 0; i < TypeRegistry::get_num_registered_types(); ++i) {
				data[i] = other.data[i];
			}
		}
	}

	constexpr ~TypeArray() {
		delete[] data;
	}

	template <typename Self>
	[[nodiscard]] FORCEINLINE constexpr auto operator[](this Self&& self, const IdType id) -> decltype(auto) {
		ASSERT(id.is_valid());
		ASSERT(id >= 0);
		ASSERTF(!self.is_empty(), "Attempted to get index {} from empty TypeArray!", id.get_value());
		return (std::forward_like<Self>(self.data[id.get_value()]));
	}

	[[nodiscard]] FORCEINLINE constexpr auto is_empty() const -> bool {
		return !data;
	}

	FORCEINLINE constexpr auto reset() -> void {
		delete[] data;
		data = null;
	}

	template <typename Self>
	[[nodiscard]] FORCEINLINE constexpr auto begin(this Self&& self) -> decltype(&std::forward<Self>(self)[0]) {
		return self.data;
	}

	template <typename Self>
	[[nodiscard]] FORCEINLINE constexpr auto end(this Self&& self) -> decltype(&std::forward<Self>(self)[0]) {
		return self.data + TypeRegistry::get_num_registered_types();
	}

	T* data;
};

template <cpts::IntAlias IdType, typename T>
struct TypeSpan : public Span<T> {
	constexpr TypeSpan() = default;
	constexpr TypeSpan(const TypeSpan&) = default;
	constexpr TypeSpan(TypeSpan&&) noexcept = default;
	constexpr auto operator=(const TypeSpan&) -> TypeSpan& = default;
	constexpr auto operator=(TypeSpan&&) noexcept -> TypeSpan& = default;

	auto operator[](usize) -> T& = delete;

	using Span<T>::Span;

	[[nodiscard]] FORCEINLINE constexpr auto operator[](const IdType id) -> T& {
		return Span<T>::operator[](id.get_value());
	}

	template <typename Other> requires (std::convertible_to<T*, Other*> && sizeof(T) == sizeof(Other))
	[[nodiscard]] FORCEINLINE constexpr operator TypeSpan<IdType, Other>&() {
		return *reinterpret_cast<TypeSpan<IdType, Other>*>(this);
	}

	template <typename Other> requires (std::convertible_to<T*, Other*> && sizeof(T) == sizeof(Other))
	[[nodiscard]] FORCEINLINE constexpr operator const TypeSpan<IdType, Other>&() const {
		return *reinterpret_cast<TypeSpan<IdType, Other>*>(this);
	}
};

// @TODO: Should take an Allocator argument.
// Uses a single allocation to manage multiple different types with SoA memory layout.
// Automatically allocates and default-constructs on instantiation.
template <cpts::IntAlias IdType, typename... Ts>
struct TypeMultiArray {
	using TypeRegistry = TypeRegistry<IdType>;

	TypeMultiArray() {
		data = mem::alloc(get_size(), get_allocation_alignment());
		utils::make_index_sequence_param_pack<sizeof...(Ts)>([&]<usize... Is>() {
			([&] {
				using T = utils::TypeAtIndex<Is, Ts...>;
				const Span<T> span = get_view<Is>();

				if constexpr (!std::is_trivially_constructible_v<T>) {
					for (T& value : span) {
						std::construct_at(&value);
					}
				}
			}(), ...);
		});
	}

	constexpr TypeMultiArray(NoInit) {}

	TypeMultiArray(TypeMultiArray&& other) noexcept {
		if (this == &other) [[unlikely]] {
			return;
		}

		if (other.is_empty()) {
			data = null;
			return;
		}

		data = other.data;
		other.data = null;
	}

	auto operator=(TypeMultiArray&& other) noexcept -> TypeMultiArray& {
		if (this == &other) [[unlikely]] {
			return *this;
		}

		reset();

		if (other.is_empty()) {
			return *this;
		}

		data = other.data;
		other.data = null;

		return *this;
	}

	~TypeMultiArray() {
		reset();
	}

	auto reset() -> bool {
		if (is_empty()) {
			return false;
		}

		utils::make_index_sequence_param_pack<sizeof...(Ts)>([&]<usize... Is>() {
			([&] {
				using T = utils::TypeAtIndex<Is, Ts...>;
				if constexpr (!std::is_trivially_destructible_v<T>) {
					const Span<T> span = get_view<Is>();
					for (T& value : span) {
						std::destroy_at(&value);
					}
				}
			}(), ...);
		});

		mem::dealloc(data, get_allocation_alignment());
		data = null;

		return true;
	}

	[[nodiscard]] FORCEINLINE constexpr auto is_empty() const -> bool {
		return !data;
	}

	[[nodiscard]] FORCEINLINE static constexpr auto get_alignment() -> usize {
		usize max_alignment = 0;
		((max_alignment = std::max(max_alignment, alignof(Ts))), ...);
		return max_alignment;
	}

	[[nodiscard]] FORCEINLINE static auto get_size() -> usize {
		static const usize VALUE = [] {
			usize aggregate_size = 0;
			([&] {
				aggregate_size = math::align(aggregate_size, alignof(Ts));
				aggregate_size += TypeRegistry::get_num_registered_types() * sizeof(Ts);
			}(), ...);
			return aggregate_size;
		}();
		return VALUE;
	}

	template <usize I>
	[[nodiscard]] FORCEINLINE static auto get_offset() -> usize {
		static const usize OFFSET = [] {
			usize offset = 0;
			[&]<usize INDEX>(this auto&& self) -> void {
				using T = utils::TypeAtIndex<INDEX, Ts...>;
				offset = math::align(offset, alignof(T));
				if constexpr (INDEX < I) {
					offset += TypeRegistry::get_num_registered_types() * sizeof(T);

					self.template operator()<INDEX + 1>();
				}
			}.template operator()<0>();
			return offset;
		}();
		return OFFSET;
	}

	template <usize I> requires (I < sizeof...(Ts))
	[[nodiscard]] FORCEINLINE auto get_view() -> TypeSpan<IdType, utils::TypeAtIndex<I, Ts...>> {
		ASSERT(!is_empty());

		using T = utils::TypeAtIndex<I, Ts...>;
		return TypeSpan<IdType, T>{reinterpret_cast<T*>(static_cast<u8*>(data) + get_offset<I>()), TypeRegistry::get_num_registered_types()};
	}

	template <usize I> requires (I < sizeof...(Ts))
	[[nodiscard]] FORCEINLINE auto get_view() const -> TypeSpan<IdType, const utils::TypeAtIndex<I, Ts...>> {
		return const_cast<TypeMultiArray*>(this)->get_view<I>();
	}

	template <usize I, typename Self> requires (I < sizeof...(Ts))
	[[nodiscard]] FORCEINLINE auto get(this Self&& self, const IdType id) -> decltype(auto) {
		ASSERT(id.is_valid());
		ASSERT(id >= 0);
		ASSERTF(!self.is_empty(), "Attempted to get index {} from empty TypeMultiArray!", id.get_value());
		return (std::forward_like<Self>(self.template get_view<I>()[id]));
	}

	template <typename T>
	[[nodiscard]] static consteval auto num_instances_of() -> usize {
		usize count = 0;
		((count += std::same_as<T, Ts>), ...);
		return count;
	}

	template <typename T, typename Self> requires (num_instances_of<T>() == 1)
	[[nodiscard]] FORCEINLINE auto get_view(this Self&& self) -> decltype(auto) {
		return (std::forward<Self>(self).template get_view<utils::index_of_type<T, Ts...>()>());
	}

	template <typename T, typename Self> requires (num_instances_of<T>() == 1)
	[[nodiscard]] FORCEINLINE auto get(this Self&& self, const IdType id) -> decltype(auto) {
		return (std::forward<Self>(self).template get<utils::index_of_type<T, Ts...>()>(id));
	}

	void* data;
};
}

template <cpts::IntAlias IdType> requires (utils::get_raw_type_name<IdType>().contains("ecs::"))
struct fmt::formatter<IdType> {
	constexpr auto parse(fmt::format_parse_context& context) {
		return context.begin();
	}

	template <typename FmtContext>
	auto format(const IdType& value, FmtContext& context) {
		return fmt::format_to(context.out(), "{}", ecs::get_type_info(value).name);
	}
};

template <usize N, cpts::IntAlias IdType>
struct fmt::formatter<ecs::StaticTypeMask<IdType, N>> {
	constexpr auto parse(fmt::format_parse_context& context) {
		return context.begin();
	}

	template <typename FmtContext>
	auto format(const ecs::StaticTypeMask<IdType, N>& value, FmtContext& context) {
		String out;
		bool is_first = true;
		value.for_each([&](const IdType id) {
			if (is_first) {
				is_first = false;
			} else {
				out += ", ";
			}

			out += fmt::format("{}", id);
		});
		return fmt::format_to(context.out(), "{{ {} }}", out);
	}
};
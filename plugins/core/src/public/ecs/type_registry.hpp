#pragma once

#include "types.hpp"
#include "rtti.hpp"
#include "static_bitmask.hpp"
#include "bitmask.hpp"

namespace ecs {
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
		});
	}

	FORCEINLINE constexpr auto for_each_with_break(::cpts::InvokableReturns<bool, IdType> auto&& fn) const -> bool {
		return mask.for_each_set_bit_with_break([&](const usize i) {
			return std::invoke(fn, IdType{i});
		});
	}

	FORCEINLINE constexpr auto negate() -> StaticTypeMask& {
		mask.negate();
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
		mask[id.get_value()] = true;
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
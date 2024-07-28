#pragma once

#include "types.hpp"
#include "rtti.hpp"
#include "static_bitmask.hpp"

namespace ecs {
template <cpts::IntAlias IdType>
struct TypeRegistry {
	EXPORT_API inline static Array<const rtti::TypeInfo*> TYPE_INFOS;

private:
	template <typename T>
	struct TypeInstantiator {
		EXPORT_API inline static const IdType ID = [] {
			const usize index = TYPE_INFOS.size();
			ASSERTF(index < IdType::max(), "TypeRegistry for type {} has exceeded {} registered types!", utils::get_type_name<T>(), IdType::max());
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
		return *TYPE_INFOS[id.get_value()];
	}

	[[nodiscard]] FORCEINLINE static auto get_num_registered_types() -> usize {
		return TYPE_INFOS.size();
	}
};

template <cpts::IntAlias IdType, usize N, std::unsigned_integral Word = SizedUnsignedIntegral<N>>
struct StaticTypeMask {
	using TypeRegistry = TypeRegistry<IdType>;

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

	[[nodiscard]] FORCEINLINE constexpr auto has(const IdType id) const -> bool {
		return mask[id.get_value()];
	}

	template <typename T>
	FORCEINLINE auto add() -> StaticTypeMask& {
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

	[[nodiscard]] FORCEINLINE constexpr auto operator==(const StaticTypeMask& other) const -> bool {
		return mask == other.mask;
	}

	[[nodiscard]] FORCEINLINE constexpr auto operator&=(const StaticTypeMask& other) -> StaticTypeMask& {
		mask &= other.mask;
		return *this;
	}

	[[nodiscard]] FORCEINLINE constexpr auto operator|=(const StaticTypeMask& other) -> StaticTypeMask& {
		mask |= other.mask;
		return *this;
	}

	[[nodiscard]] FORCEINLINE constexpr auto operator^=(const StaticTypeMask& other) -> StaticTypeMask& {
		mask ^= other.mask;
		return *this;
	}

	[[nodiscard]] FORCEINLINE friend constexpr auto operator&(const StaticTypeMask& a, const StaticTypeMask& b) -> StaticTypeMask {
		return auto{a} &= b;
	}

	[[nodiscard]] FORCEINLINE friend constexpr auto operator|(const StaticTypeMask& a, const StaticTypeMask& b) -> StaticTypeMask {
		return auto{a} |= b;
	}

	[[nodiscard]] FORCEINLINE friend constexpr auto operator^(const StaticTypeMask& a, const StaticTypeMask& b) -> StaticTypeMask {
		return auto{a} ^= b;
	}

	StaticBitMask<N, Word> mask;
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
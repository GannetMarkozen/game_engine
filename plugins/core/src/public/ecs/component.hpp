#pragma once

#include "../defines.hpp"
#include "../types.hpp"
#include <type_traits>

struct ComponentId : public IntAlias<u16> {
	using IntAlias<u16>::IntAlias;
};

template<typename T>
struct ComponentTraitsBase {
	static consteval fn get_name() -> StringView {
		return get_type_name<T>();
	}

	static fn construct(void* dst, const usize count) -> void {
		static_assert(std::is_default_constructible_v<T>, "Component type must be default constructible or you must override ComponentTraits::construct.");
		for (usize i = 0; i < count; ++i) {
			new(&static_cast<T*>(dst)[i]) T{};
		}
	};

	static fn copy_construct(void* dst, const void* src, const usize count) -> void {
		if constexpr (std::is_trivially_copy_constructible_v<T>) {
			memcpy(dst, src, sizeof(T) * count);
		} else {
			for (usize i = 0; i < count; ++i) {
				new(&static_cast<T*>(src)[i]) T{static_cast<const T*>(src)[i]};
			}
		}
	}

	static fn move_construct(void* dst, void* src, const usize count) -> void {
		if constexpr (std::is_trivially_move_constructible_v<T>) {
			memcpy(dst, src, sizeof(T) * count);
		} else if constexpr (!std::is_move_constructible_v<T>) {
			copy_construct(dst, src, count);
		} else {
			for (usize i = 0; i < count; ++i) {
				new(&static_cast<T*>(dst)) T{std::move(static_cast<T*>(src)[i])};
			}
		}
	}

	static fn copy_assign(void* dst, const void* src, const usize count) -> void {
		if constexpr (std::is_trivially_copy_assignable_v<T>) {
			memcpy(dst, src, sizeof(T) * count);
		} else {
			for (usize i = 0; i < count; ++i) {
				static_cast<T*>(dst)[i] = static_cast<const T*>(src)[i];
			}
		}
	};

	static fn move_assign(void* dst, void* src, const usize count) -> void {
		if constexpr (std::is_trivially_move_assignable_v<T>) {
			memcpy(dst, src, sizeof(T) * count);
		} else if constexpr (!std::is_move_assignable_v<T>) {
			copy_construct(dst, src, count);
		} else {
			for (usize i = 0; i < count; ++i) {
				static_cast<T*>(dst)[i] = std::move(static_cast<T*>(src)[i]);
			}
		}
	};

	static fn destructor(void* dst, const usize count) -> void {
		if constexpr (!std::is_trivially_destructible_v<T>) {
			for (usize i = 0; i < count; ++i) {
				static_cast<T*>(dst)[i].~T();
			}
		}
	};
};

template<typename T>
struct ComponentTraits : public ComponentTraitsBase<T> {};

namespace core::ecs {
	struct ComponentRecord {
		String name;
		void(*construct)(void*, usize);
		void(*copy_construct)(void*, const void*, usize);
		void(*move_construct)(void*, void*, usize);
		void(*copy_assign)(void*, const void*, usize);
		void(*move_assign)(void*, void*, usize);
		void(*destructor)(void*, usize);
		usize size;
		usize alignment;
		u64 is_tag: 1;
	};

namespace impl {
EXPORT_API inline Array<ComponentRecord> component_records;
}

template<typename T>
[[nodiscard]]
inline fn get_component_id() -> ComponentId {
	static const ComponentId id = [] {
		const auto old_size = impl::component_records.size();
		ASSERT(old_size < UINT16_MAX);

		using Traits = ComponentTraits<T>;
		impl::component_records.push_back(ComponentRecord{
			.name = Traits::get_name(),
			.construct = &Traits::construct,
			.copy_construct = &Traits::copy_construct,
			.move_construct = &Traits::move_construct,
			.copy_assign = &Traits::copy_assign,
			.move_assign = &Traits::move_assign,
			.destructor = &Traits::destructor,
			.size = sizeof(T),
			.alignment = alignof(T),
			.is_tag = std::is_empty_v<T>,
		});

		return ComponentId{static_cast<u16>(old_size)};
	}();

	return id;
}

[[nodiscard]]
FORCEINLINE fn get_component_info(const ComponentId component) -> const ComponentRecord& {
	return impl::component_records[component];
}

// May change at runtime.
[[nodiscard]]
inline fn get_component_count() -> u32 {
	return impl::component_records.size();
}

struct ComponentMask : public BitArray<> {
	using BitArray<>::BitArray;

	template<typename... Ts>
	requires (sizeof...(Ts) > 0)
	FORCEINLINE fn add() -> ComponentMask& {
		(insert(get_component_id<Ts>().get_value(), true), ...);
		return *this;
	}

	template<typename... Ts>
	requires (sizeof...(Ts) > 0)
	FORCEINLINE fn remove() -> ComponentMask& {
		(insert(get_component_id<Ts>().get_value(), false), ...);
		return *this;
	}

	template<typename T>
	[[nodiscard]]
	FORCEINLINE fn has() const -> bool {
		const auto id = get_component_id<T>().get_value();
		return is_valid_index(id) && (*this)[id];
	}
};
}
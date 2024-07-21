#pragma once

#include "static_reflection.hpp"
#include "memory.hpp"

namespace rtti {
struct TypeId : public IntAlias<u16> {
	using IntAlias<u16>::IntAlias;
};

constexpr TypeId NULL_TYPE_ID{UINT16_MAX};

enum class TypeFlags : u8 {
	NONE = 0,
	TRIVIAL = 1 << 0,
	REFLECTED = 1 << 1,
};
}

DECLARE_ENUM_CLASS_FLAGS(rtti::TypeFlags);

namespace rtti {
struct Any;

struct Attribute {
	String name;
};

struct TypeInfo {
	String name;

	usize size, alignment;

	TypeFlags flags;

	void(*construct)(void*, usize);
	void(*copy_construct)(void*, const void*, usize);
	void(*move_construct)(void*, void*, usize);
	void(*copy_assign)(void*, const void*, usize);
	void(*move_assign)(void*, void*, usize);
	void(*destruct)(void*, usize);

	void(*serialize_json)(std::ostream&, const void*);
	void(*deserialize_json)(std::istream&, void*);

	Optional<TypeId> parent;
};

namespace impl {
EXPORT_API inline Array<TypeInfo> type_infos;

template <typename T>
struct TypeInfoInstantiator final {
	EXPORT_API inline static const TypeId TYPE_ID = [] {
		const usize index = type_infos.size();
		ASSERT(index < UINT16_MAX);

		type_infos.push_back(TypeInfo{
			.name = String{utils::get_raw_type_name<T>()},
			.size = sizeof(T),
			.alignment = alignof(T),
			.flags = [] {
				TypeFlags out;
				if constexpr (std::is_trivial_v<T>) out |= TypeFlags::TRIVIAL;
				if constexpr (cpts::Reflected<T>) out |= TypeFlags::REFLECTED;
				return out;
			}(),
			.construct = [](void* dst, const usize count) {
				if constexpr (!std::is_trivially_constructible_v<T>) {
					for (usize i = 0; i < count; ++i) {
						new(&static_cast<T*>(dst)[i]) T{};
					}
				}
			},
			.copy_construct = [](void* dst, const void* src, const usize count) {
				if constexpr (std::is_trivially_copy_constructible_v<T>) {
					memcpy(dst, src, count * sizeof(T));
				} else {
					for (usize i = 0; i < count; ++i) {
						new(&static_cast<T*>(dst)[i]) T{static_cast<const T*>(src)[i]};
					}
				}
			},
			.move_construct = [](void* dst, void* src, const usize count) {
				if constexpr (std::is_trivially_move_constructible_v<T>) {
					memcpy(dst, src, count * sizeof(T));
				} else {
					for (usize i = 0; i < count; ++i) {
						new(&static_cast<T*>(dst)[i]) T{std::move(static_cast<T*>(src)[i])};
					}
				}
			},
			.copy_assign = [](void* dst, const void* src, const usize count) {
				if constexpr (std::is_trivially_copy_assignable_v<T>) {
					memcpy(dst, src, count * sizeof(T));
				} else {
					for (usize i = 0; i < count; ++i) {
						static_cast<T*>(dst)[i] = static_cast<const T*>(src)[i];
					}
				}
			},
			.move_assign = [](void* dst, void* src, const usize count) {
				if constexpr (std::is_trivially_move_assignable_v<T>) {
					memcpy(dst, src, count * sizeof(T));
				} else {
					for (usize i = 0; i < count; ++i) {
						static_cast<T*>(dst)[i] = std::move(static_cast<T*>(src)[i]);
					}
				}
			},
			.destruct = [](void* dst, const usize count) {
				if constexpr (!std::is_trivially_destructible_v<T>) {
					for (usize i = 0; i < count; ++i) {
						static_cast<T*>(dst)[i].~T();
					}
				}
			},
			.serialize_json = [](std::ostream& stream, const void* src) {
				serialize_json(stream, *static_cast<const T*>(src));
			}
		});

		return TypeId{static_cast<u16>(index)};
	}();
};
}

template <typename T>
[[nodiscard]] FORCEINLINE auto get_type_id() -> TypeId {
	return impl::TypeInfoInstantiator<T>::TYPE_ID;
}

[[nodiscard]] FORCEINLINE auto get_type_info(const TypeId id) -> const TypeInfo& {
	return impl::type_infos[id];
}

template <typename T>
[[nodiscard]] FORCEINLINE auto get_type_info() -> const TypeInfo& {
	return get_type_info(get_type_id<T>());
}
}

// @TODO: Add small size optimizations / allocators.
struct Any {
	FORCEINLINE constexpr Any()
		: type{rtti::NULL_TYPE_ID} {}

	FORCEINLINE constexpr explicit Any(NoInit) {}

	Any(const Any& other) {
		if ((type = other.type) == rtti::NULL_TYPE_ID) {
			data = null;
			return;
		}

		const auto& type_info = rtti::get_type_info(type);
		data = mem::alloc(type_info.size, type_info.alignment);

		type_info.copy_construct(data, other.data, 1);
	}

	FORCEINLINE constexpr Any(Any&& other) noexcept {
		if (this == &other) [[unlikely]] {
			return;
		}

		data = other.data;
		type = other.type;

		other.data = null;
		other.type = rtti::NULL_TYPE_ID;
	}

	auto operator=(const Any& other) -> Any& {
		reset();
		return *new(this) Any{other};
	}

	FORCEINLINE auto operator=(Any&& other) noexcept -> Any& {
		if (this == &other) [[unlikely]] {
			return *this;
		}

		reset();

		data = other.data;
		type = other.type;

		other.data = null;
		other.type = rtti::NULL_TYPE_ID;

		return *this;
	}

	FORCEINLINE ~Any() {
		reset();
	}

	template <typename T, typename... Args> requires std::constructible_from<T, Args&&...>
	[[nodiscard]] FORCEINLINE static auto make(Args&&... args) -> Any {
		Any out{NO_INIT};
		//out.data = new T{std::forward<Args>(args)...};
		out.data = new(mem::alloc(sizeof(T), alignof(T))) T{std::forward<Args>(args)...};
		out.type = rtti::get_type_id<T>();
		return out;
	}

	FORCEINLINE auto reset() -> bool {
		if (type == rtti::NULL_TYPE_ID) {
			return false;
		}

		ASSERT(!!data);

		const auto& type_info = rtti::get_type_info(type);
		type_info.destruct(data, 1);

		mem::dealloc(data, type_info.alignment);
		data = null;
		type = rtti::NULL_TYPE_ID;

		return true;
	}

	[[nodiscard]] FORCEINLINE auto has_value() const -> bool {
		return type != rtti::NULL_TYPE_ID;
	}

	[[nodiscard]] FORCEINLINE auto get_type() const -> rtti::TypeId {
		return type;
	}

	template <typename T>
	FORCEINLINE auto is() const -> bool {
		return type == rtti::get_type_id<T>();
	}

	template <typename T, typename... Args> requires std::constructible_from<T, Args&&...>
	FORCEINLINE auto emplace(Args&&... args) -> T& {
		reset();

		data = new(mem::alloc(sizeof(T), alignof(T))) T{std::forward<Args>(args)...};
		type = rtti::get_type_id<T>();

		return *static_cast<T*>(data);
	}

	template <typename T>
	FORCEINLINE auto set(T&& value) -> decltype(auto) {
		return (emplace<std::decay_t<T>>(std::forward<T>(value)));
	}

	[[nodiscard]] FORCEINLINE auto get_data() -> void* {
		return data;
	}

	[[nodiscard]] FORCEINLINE auto get_data() const -> const void* {
		return data;
	}

	template <typename T, typename Self>
	[[nodiscard]] FORCEINLINE auto as(this Self&& self) -> decltype(auto) {
		ASSERT(self.has_value());
		ASSERTF(self.template is<T>(), "Could not cast {} to {}!", rtti::get_type_info(self.type).name, utils::get_raw_type_name<T>());
		return (std::forward_like<Self>(*(T*)self.data));
	}

	template <typename T, typename Self>
	[[nodiscard]] FORCEINLINE auto try_as(this Self&& self) -> auto* {
		return self.template is<T>() ? &self.template as<T>() : null;
	}

private:
	void* data;
	rtti::TypeId type;
};
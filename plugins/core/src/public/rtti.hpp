#pragma once

#include "static_reflection.hpp"
#include "serialization.hpp"
#include "memory.hpp"

#include <concepts>
#include <thread>

namespace rtti {
struct TypeInfo;

template <typename>
auto get_type_info() -> const TypeInfo&;
}

// @TODO: Implement small buffer optimizations or allocators.
struct Any {
	FORCEINLINE constexpr Any()
		: data{null}, type_info{null} {}

	FORCEINLINE constexpr explicit Any(NoInit) {}

	FORCEINLINE Any(const Any& other);
	FORCEINLINE Any(Any&& other) noexcept;

	FORCEINLINE auto operator=(const Any& other) -> Any&;
	FORCEINLINE auto operator=(Any&& other) noexcept -> Any&;

	template <typename T>
	FORCEINLINE explicit Any(T&& value);

	FORCEINLINE ~Any();

	FORCEINLINE auto reset() -> bool;

	template <typename T, typename... Args> requires std::constructible_from<T, Args&&...>
	FORCEINLINE auto emplace(Args&&... args) -> T& {
		reset();

		data = new T{std::forward<Args>(args)...};
		type_info = &rtti::get_type_info<T>();

		return *static_cast<T*>(data);
	}

	[[nodiscard]] FORCEINLINE auto get_data() -> void* {
		return data;
	}

	[[nodiscard]] FORCEINLINE auto get_data() const -> const void* {
		return data;
	}

	[[nodiscard]] FORCEINLINE auto get_type() const -> const rtti::TypeInfo* {
		return type_info;
	}

	[[nodiscard]] FORCEINLINE auto has_value() const -> bool {
		return !!type_info;
	}

	[[nodiscard]] FORCEINLINE auto is(const rtti::TypeInfo& other) const -> bool {
		return type_info == &other;
	}

	// @TODO: Optimize cache-misses for this.
	[[nodiscard]] FORCEINLINE auto is_child_of(const rtti::TypeInfo& other) const -> bool;

	template <typename T>
	[[nodiscard]] FORCEINLINE auto is() const -> bool {
		return is(rtti::get_type_info<T>());
	}

	template <typename T>
	[[nodiscard]] FORCEINLINE auto is_child_of() const -> bool {
		return is_child_of(rtti::get_type_info<T>());
	}

	template <typename T, typename Self>
	[[nodiscard]] FORCEINLINE auto as(this Self&& self) -> decltype(auto);

	template <typename T, bool INCLUDE_SUPER = true>
	[[nodiscard]] FORCEINLINE auto try_as() -> T* {
		return [&] {
			if constexpr (INCLUDE_SUPER) {
				return is_child_of<T>();
			} else {
				return is<T>();
			}
		}() ? static_cast<T*>(data) : null;
	}

	template <typename T, bool INCLUDE_SUPER = true>
	[[nodiscard]] FORCEINLINE auto try_as() const -> const T* {
		return const_cast<Any*>(this)->try_as<T, INCLUDE_SUPER>();
	}

private:
	void* data;
	const rtti::TypeInfo* type_info;
};

namespace rtti {
enum class Flags : u8 {
	NONE = 0,
	TRIVIAL = 1 << 0,
	TRIVIALLY_RELOCATABLE = 1 << 1,// Whether this type can be memcpy'd into a new destination (instead of move constructed then destructed).
	EQUALITY_COMPARABLE = 1 << 2,
	HASHABLE = 1 << 3,
	SERIALIZABLE = 1 << 4,
	REFLECTED = 1 << 5,// Has any reflected properties.
	EMPTY = 1 << 6,// No members.
};
}

DECLARE_ENUM_CLASS_FLAGS(rtti::Flags);

namespace rtti {
enum class Type : u8 {
	STRUCT, ENUM, FUNDAMENTAL,
};

struct Attribute {
	StringView name;
	//Any value;
};

struct Member {
	StringView name;
	usize offset;
	const TypeInfo& type;
	Array<Attribute> attributes;
};

struct Entry {
	StringView name;
	i64 value;
};

struct TypeInfo {
	StringView name;
	usize size, alignment, alloc_alignment;
	Flags flags;
	Type type;

	void(*construct)(void*, usize);
	void(*copy_construct)(void*, const void*, usize);
	void(*move_construct)(void*, void*, usize);
	void(*copy_assign)(void*, const void*, usize);
	void(*move_assign)(void*, void*, usize);
	void(*relocate_occupied)(void*, void*, usize);// Merges destruction of dst region and relocation of src region. Assumes no memory overlap.
	void(*destruct)(void*, usize);
	bool(*equals)(const void*, const void*);
	usize(*hash)(const void*);

	const TypeInfo* parent;// Potentially NULL.
	Array<Attribute> attributes;
	Array<Member> members;
	Array<Entry> entries;
};

template <typename T> requires (!std::is_const_v<T> && !std::is_volatile_v<T> && !std::is_reference_v<T>)
EXPORT_API inline const TypeInfo TYPE_INFO{
	.name = utils::get_type_name<T>(),
	.size = sizeof(T),
	.alignment = alignof(T),
	.alloc_alignment = alignof(T) > mem::DEFAULT_ALIGNMENT_SIZE ? alignof(T) : mem::DEFAULT_ALIGNMENT,
	.flags = [] {
		Flags out = Flags::NONE;
		if constexpr (std::is_trivial_v<T>) out |= Flags::TRIVIAL;
		if constexpr (cpts::TriviallyRelocatable<T>) out |= Flags::TRIVIALLY_RELOCATABLE;
		if constexpr (std::equality_comparable<T>) out |= Flags::EQUALITY_COMPARABLE;
		if constexpr (cpts::JsonSerializable<T>) out |= Flags::SERIALIZABLE;
		if constexpr (requires { { std::hash<T>{}(std::declval<const T>()) } -> std::same_as<usize>; }) out |= Flags::HASHABLE;
		if constexpr (cpts::Reflected<T>) out |= Flags::REFLECTED;
		if constexpr (std::is_empty_v<T>) out |= Flags::EMPTY;
		return out;
	}(),
	.type = []() -> Type {
		if constexpr (cpts::Struct<T>) {
			return Type::STRUCT;
		} else if constexpr (cpts::Enum<T>) {
			return Type::ENUM;
		} else if constexpr (cpts::Fundamental<T>) {
			return Type::FUNDAMENTAL;
		} else {
			static_assert(false, "Unknown class of type!");
		}
	}(),
	.construct = [](void* dst, const usize count) {
		if constexpr (std::is_constructible_v<T>) {
			for (usize i = 0; i < count; ++i) {
				std::construct_at(&static_cast<T*>(dst)[i]);
			}
		} else {
			UNIMPLEMENTED;
		}
	},
	.copy_construct = [](void* dst, const void* src, const usize count) {
		if constexpr (std::is_copy_constructible_v<T>) {
			if constexpr (std::is_trivially_copy_constructible_v<T>) {
				memcpy(dst, src, count * sizeof(T));
			} else {
				for (usize i = 0; i < count; ++i) {
					std::construct_at(&static_cast<T*>(dst)[i], static_cast<const T*>(src)[i]);
				}
			}
		} else {
			UNIMPLEMENTED;
		}
	},
	.move_construct = [](void* dst, void* src, const usize count) {
		if constexpr (std::is_move_constructible_v<T>) {
			if constexpr (std::is_trivially_move_constructible_v<T>) {
				memcpy(dst, src, count * sizeof(T));
			} else {
				for (usize i = 0; i < count; ++i) {
					std::construct_at(&static_cast<T*>(dst)[i], std::move(static_cast<T*>(src)[i]));
				}
			}
		} else {
			UNIMPLEMENTED;
		}
	},
	.copy_assign = [](void* dst, const void* src, const usize count) {
		if constexpr (std::is_copy_assignable_v<T>) {
				if constexpr (std::is_trivially_copy_assignable_v<T>) {
				memcpy(dst, src, count * sizeof(T));
			} else {
				for (usize i = 0; i < count; ++i) {
					static_cast<T*>(dst)[i] = static_cast<const T*>(src)[i];
				}
			}
		} else {
			UNIMPLEMENTED;
		}
	},
	.move_assign = [](void* dst, void* src, const usize count) {
		if constexpr (std::is_move_assignable_v<T>) {
				if constexpr (std::is_trivially_move_assignable_v<T>) {
				memcpy(dst, src, count * sizeof(T));
			} else {
				for (usize i = 0; i < count; ++i) {
					static_cast<T*>(dst)[i] = std::move(static_cast<T*>(src)[i]);
				}
			}
		} else {
			UNIMPLEMENTED;
		}
	},
	.relocate_occupied = [](void* dst, void* src, const usize count) {
		if constexpr (cpts::TriviallyRelocatable<T>) {
			if constexpr (std::is_trivially_destructible_v<T>) {
				memcpy(dst, src, count * sizeof(T));
			} else {
				for (usize i = 0; i < count; ++i) {
					static_cast<T*>(dst)[i].~T();
					memcpy(&static_cast<T*>(dst)[i], &static_cast<T*>(src)[i], sizeof(T));
				}
			}
		} else if constexpr (std::is_move_constructible_v<T>) {
			for (usize i = 0; i < count; ++i) {
				static_cast<T*>(dst)[i].~T();
				std::construct_at(&static_cast<T*>(dst)[i], std::move(static_cast<T*>(src)[i]));
				static_cast<T*>(src)[i].~T();
			}
		} else {
			UNIMPLEMENTED;
		}
	},
	.destruct = [](void* dst, const usize count) {
		if constexpr (std::is_destructible_v<T> && !std::is_trivially_destructible_v<T>) {
			for (usize i = 0; i < count; ++i) {
				std::destroy_at(&static_cast<T*>(dst)[i]);
			}
		}
	},
	.equals = [](const void* a, const void* b) -> bool {
#if 0
		if constexpr (std::equality_comparable<T>) {
			return *static_cast<const T*>(a) == *static_cast<const T*>(b);
		} else {
			UNIMPLEMENTED;
		}
#endif
		UNIMPLEMENTED;
	},
	.hash = [](const void* src) -> usize {
		if constexpr (requires { { std::hash<T>{}(std::declval<const T>()) } -> std::same_as<usize>; }) {
			return std::hash<T>{}(*static_cast<const T*>(src));
		} else {
			UNIMPLEMENTED;
		}
	},
	.parent = []() -> const TypeInfo* {
		if constexpr (cpts::ReflectedParent<T>) {
			return &get_type_info<typename Reflect<T>::Parent>();
		} else {
			return null;
		}
	}(),
	.attributes = [] {
		Array<Attribute> out;
		if constexpr (cpts::ReflectedAttributes<T>) {
			const auto& attributes = Reflect<T>::Attributes::ATTRIBUTES;
			out.reserve(attributes.count());
			attributes([&]<usize N, typename ValueType>(const ::Attribute<N, ValueType>& attribute) {
				out.push_back(Attribute{
					.name = attribute.name.view(),
					.value{attribute.value},
				});
			});
		}
		return out;
	}(),
	.members = [] {
		Array<Member> members;
		if constexpr (cpts::ReflectedMembers<T>) {
			meta::make_reflected_members_param_pack<T>([&]<cpts::Member... Members>() {
				members.reserve(sizeof...(Members));
				(members.push_back(Member{
					.name = Members::NAME.view(),
					.offset = Members::MEMBER.get_offset(),
					.type = get_type_info<typename Members::MemberType>(),
					.attributes = [] {
						Array<Attribute> out;
						out.reserve(utils::count<decltype(Members::ATTRIBUTES)>());
						utils::visit(Members::ATTRIBUTES, [&]<usize N, typename ValueType>(const ::Attribute<N, ValueType>& attribute) {
							out.push_back(Attribute{
								.name = attribute.name.view(),
								//.value{attribute.value},
							});
						});
						return out;
					}(),
				}), ...);
			});
		}
		return members;
	}(),
};

template <typename T>
[[nodiscard]] FORCEINLINE auto get_type_info() -> const TypeInfo& {
	return TYPE_INFO<T>;
}
}

FORCEINLINE Any::Any(const Any& other) {
	if (!!(type_info == other.type_info)) {
		data = mem::alloc(type_info->size, type_info->alloc_alignment);
		type_info->copy_construct(data, other.data, 1);
	} else {
		data = null;
	}
}

FORCEINLINE Any::Any(Any&& other) noexcept {
	if (this == &other) [[unlikely]] {
		return;
	}

	data = other.data;
	type_info = other.type_info;

	other.data = null;
	other.type_info = null;
}

template <typename T>
FORCEINLINE Any::Any(T&& value) {
	using Type = std::decay_t<T>;

	data = new Type{std::forward<T>(value)};
	type_info = &rtti::get_type_info<Type>();
}

FORCEINLINE Any::~Any() {
	if (type_info) {
		type_info->destruct(data, 1);
		fmt::println("Deallocing {}", type_info->name);
		mem::dealloc(data, type_info->alloc_alignment);
	}
}

FORCEINLINE auto Any::operator=(const Any& other) -> Any& {
	reset();
	return *new(this) Any{other};
}

FORCEINLINE auto Any::operator=(Any&& other) noexcept -> Any& {
	if (this == &other) [[unlikely]] {
		return *this;
	}

	data = other.data;
	type_info = other.type_info;

	other.data = null;
	other.type_info = null;

	return *this;
}

FORCEINLINE auto Any::reset() -> bool {
	if (!type_info) {
		ASSERT(!data);
		return false;
	}

	ASSERT(data);

	type_info->destruct(data, 1);
	mem::dealloc(data, type_info->alloc_alignment);

	data = null;
	type_info = null;

	return true;
}

[[nodiscard]] FORCEINLINE auto Any::is_child_of(const rtti::TypeInfo& other) const -> bool {
	const rtti::TypeInfo* current_type = type_info;
	while (current_type) {
		if (current_type == &other) {
			return true;
		}
		current_type = current_type->parent;
	}
	return false;
}

template <typename T, typename Self>
[[nodiscard]] FORCEINLINE auto Any::as(this Self&& self) -> decltype(auto) {
	ASSERTF(self.template is_child_of<T>(), "Attempted to cast {} to {}!", self.type_info ? self.type_info->name : "NULL", utils::get_type_name<T>());
	return (std::forward_like<Self>(*(T*)self.data));
}
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
struct EXPORT_API Any {
	FORCEINLINE constexpr Any()
		: data{null}, type_info{null} {}

	FORCEINLINE constexpr explicit Any(NoInit) {}

	FORCEINLINE Any(const Any& other);
	FORCEINLINE Any(Any&& other) noexcept;

	template <typename T>
	FORCEINLINE explicit Any(T&& value);

	FORCEINLINE ~Any();

	[[nodiscard]] FORCEINLINE auto get_data() -> void* {
		return data;
	}

	[[nodiscard]] FORCEINLINE auto get_data() const -> const void* {
		return data;
	}

	[[nodiscard]] FORCEINLINE auto get_type() const -> const rtti::TypeInfo* {
		return type_info;
	}

private:
	void* data;
	const rtti::TypeInfo* type_info;
};

namespace rtti {
enum class Flags : u8 {
	NONE = 0,
	TRIVIAL = 1 << 0,
	TRIVIALLY_RELOCATABLE = 1 << 1,
	EQUALITY_COMPARABLE = 1 << 2,
	HASHABLE = 1 << 3,
	SERIALIZABLE = 1 << 4,
	REFLECTED = 1 << 5,
};
}

DECLARE_ENUM_CLASS_FLAGS(rtti::Flags);

namespace rtti {
enum class Type : u8 {
	STRUCT, ENUM, FUNDAMENTAL,
};

struct Attribute {
	StringView name;
	Any value;
};

struct Member {
	StringView name;
	usize offset;
	const TypeInfo* type;
	Array<Attribute> attributes;
};

struct Entry {
	StringView name;
	i64 value;
};

struct alignas(CACHE_LINE_SIZE) TypeInfo {
	StringView name;
	usize size, alignment, alloc_alignment;
	Flags flags;
	Type type;

	void(*construct)(void*, usize);
	void(*copy_construct)(void*, const void*, usize);
	void(*move_construct)(void*, void*, usize);
	void(*copy_assign)(void*, const void*, usize);
	void(*move_assign)(void*, void*, usize);
	void(*destruct)(void*, usize);
	bool(*equals)(const void*, const void*);
	usize(*hash)(const void*);

	const TypeInfo* parent;
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
	.destruct = [](void* dst, const usize count) {
		if constexpr (std::is_destructible_v<T> && !std::is_trivially_destructible_v<T>) {
			for (usize i = 0; i < count; ++i) {
				static_cast<T*>(dst)[i].~T();
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
			attributes.visit([&]<usize N, typename ValueType>(const ::Attribute<N, ValueType>& attribute) {
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
					.type = &get_type_info<typename Members::MemberType>(),
					.attributes = [] {
						Array<Attribute> out;
						out.reserve(Members::ATTRIBUTES.count());
						Members::ATTRIBUTES.visit([&]<usize N, typename ValueType>(const ::Attribute<N, ValueType>& attribute) {
							out.push_back(Attribute{
								.name = attribute.name.view(),
								.value{attribute.value},
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
		mem::dealloc(data, type_info->alloc_alignment);
	}
}

#if 0
namespace rtti {
enum class Flags : u8 {
	NONE = 0,
	TRIVIAL = 1 << 0,
	REFLECTED = 1 << 1,
};
}

DECLARE_ENUM_CLASS_FLAGS(rtti::Flags);

namespace rtti {
struct TypeInfo;

enum class Type : u8 {
	STRUCT, ENUM, FUNDAMENTAL,
};

// @TODO: Could potentially optimize data layout of these.
struct Attribute {
	StringView name;
	const void* value;
	const TypeInfo* value_type;
};

struct Member {
	StringView name;
	usize offset;
	const TypeInfo* type;
	Array<Attribute> attributes;
};

struct Entry {
	String name;
	i64 value;
};

struct ReflectionInfo {
	// Potentially NULL.
	const TypeInfo* parent;

	Array<Attribute> attributes;

	Array<Member> members;// Only for structs.
	Array<Entry> entries;// Only for enums.
};

struct TypeInfo {
	String name;

	usize size, alignment;

	Flags flags;
	Type type;

	void(*construct)(void*, usize);
	void(*copy_construct)(void*, const void*, usize);
	void(*move_construct)(void*, void*, usize);
	void(*copy_assign)(void*, const void*, usize);
	void(*move_assign)(void*, void*, usize);
	void(*destruct)(void*, usize);

	void(*serialize_json)(std::ostream&, const void*);
	void(*deserialize_json)(std::istream&, void*);

	Optional<ReflectionInfo> reflection_info;
};

namespace impl {
EXPORT_API inline Array<TypeInfo> type_infos;

template <typename T>
EXPORT_API inline static const TypeId TYPE_ID = [] {
	//fmt::println("EXTERN: {}", get_type_id<std::nullptr_t>().get_value());

	type_infos.push_back(TypeInfo{
		.name = String{utils::get_type_name<T>()},
		.size = sizeof(T),
		.alignment = alignof(T),
		.flags = [] {
			Flags out;
			if constexpr (std::is_trivial_v<T>) out |= Flags::TRIVIAL;
			if constexpr (cpts::Reflected<T>) out |= Flags::REFLECTED;
			return out;
		}(),
		.type = [] {
			if constexpr (cpts::Struct<T>) {
				return Type::STRUCT;
			} else if constexpr (cpts::Enum<T>) {
				return Type::ENUM;
			} else if constexpr (std::is_fundamental_v<T>) {
				return Type::FUNDAMENTAL;
			} else {
				static_assert(false, "Unknown class of type.");
			}
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
			if constexpr (cpts::JsonSerializable<T>) {
				serialize_json(stream, *static_cast<const T*>(src));
			}
		},
		.deserialize_json = null,
		.reflection_info = []() -> Optional<ReflectionInfo> {
			if constexpr (!cpts::Reflected<T>) {
				return NULL_OPTIONAL;
			} else {
				ReflectionInfo out;
				if constexpr (cpts::ReflectedParent<T>) {
					out.parent = get_type_id<typename Reflect<T>::Parent>();
				}
				if constexpr (cpts::ReflectedMembers<T>) {
					meta::make_reflected_members_param_pack<T>([&]<typename... Members>() {
						out.members.reserve(sizeof...(Members));
						([&] {
							out.members.push_back(Member{
								.name = String{Members::NAME.view()},
								.offset = Members::MEMBER.get_offset(),
								.type = get_type_id<typename Members::MemberType>(),
								.attributes = [] {
									Array<Attribute> out;
									const auto& attributes = Members::ATTRIBUTES;
									out.reserve(attributes.count());
									attributes.visit([&]<usize N, typename ValueType>(const ::Attribute<N, ValueType>& attribute) {
										out.push_back(Attribute{
											.name = String{attribute.name.view()},
											.value = &attribute.value,
											.value_type = get_type_id<std::decay_t<decltype(attribute.value)>>(),
										});
									});
									return out;
								}(),
							});
						}(), ...);
					});
				}
				if constexpr (cpts::ReflectedAttributes<T>) {
					const auto& attributes = Reflect<T>::Attributes::ATTRIBUTES;
					out.attributes.reserve(attributes.count());
					attributes.visit([&]<usize N, typename ValueType>(const ::Attribute<N, ValueType>& attribute) {
						out.attributes.push_back(Attribute{
							.name = String{attribute.name.view()},
							.value = &attribute.value,
							.value_type = get_type_id<std::decay_t<ValueType>>(),
						});
					});
				}
				// @TODO: Reflected Entries

				return {std::move(out)};
			}
		}(),
	});

	const usize index = type_infos.size() - 1;
	ASSERT(index < UINT16_MAX);

	return TypeId{static_cast<u16>(index)};
}();
}

[[nodiscard]] FORCEINLINE auto get_num_reflected_types() -> usize {
	return impl::type_infos.size();
}

template <typename T>
[[nodiscard]] FORCEINLINE auto get_type_id() -> TypeId {
	static const TypeId ID = [] {
		impl::type_infos.push_back(TypeInfo{
			.name = String{utils::get_type_name<T>()},
			.size = sizeof(T),
			.alignment = alignof(T),
			.flags = [] {
				Flags out;
				if constexpr (std::is_trivial_v<T>) out |= Flags::TRIVIAL;
				if constexpr (cpts::Reflected<T>) out |= Flags::REFLECTED;
				return out;
			}(),
			.type = [] {
				if constexpr (cpts::Struct<T>) {
					return Type::STRUCT;
				} else if constexpr (cpts::Enum<T>) {
					return Type::ENUM;
				} else if constexpr (std::is_fundamental_v<T>) {
					return Type::FUNDAMENTAL;
				} else {
					static_assert(false, "Unknown class of type.");
				}
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
				if constexpr (cpts::JsonSerializable<T>) {
					serialize_json(stream, *static_cast<const T*>(src));
				}
			},
			.deserialize_json = null,
			.reflection_info = []() -> Optional<ReflectionInfo> {
				if constexpr (!cpts::Reflected<T>) {
					return NULL_OPTIONAL;
				} else {
					ReflectionInfo out;
					if constexpr (cpts::ReflectedParent<T>) {
						out.parent = get_type_id<typename Reflect<T>::Parent>();
					}
					if constexpr (cpts::ReflectedMembers<T>) {
						meta::make_reflected_members_param_pack<T>([&]<typename... Members>() {
							out.members.reserve(sizeof...(Members));
							([&] {
								out.members.push_back(Member{
									.name = String{Members::NAME.view()},
									.offset = Members::MEMBER.get_offset(),
									.type = get_type_id<typename Members::MemberType>(),
									.attributes = [] {
										Array<Attribute> out;
										const auto& attributes = Members::ATTRIBUTES;
										out.reserve(attributes.count());
										attributes.visit([&]<usize N, typename ValueType>(const ::Attribute<N, ValueType>& attribute) {
											out.push_back(Attribute{
												.name = String{attribute.name.view()},
												.value = &attribute.value,
												.value_type = get_type_id<std::decay_t<decltype(attribute.value)>>(),
											});
										});
										return out;
									}(),
								});
							}(), ...);
						});
					}
					if constexpr (cpts::ReflectedAttributes<T>) {
						const auto& attributes = Reflect<T>::Attributes::ATTRIBUTES;
						out.attributes.reserve(attributes.count());
						attributes.visit([&]<usize N, typename ValueType>(const ::Attribute<N, ValueType>& attribute) {
							out.attributes.push_back(Attribute{
								.name = String{attribute.name.view()},
								.value = &attribute.value,
								.value_type = get_type_id<std::decay_t<ValueType>>(),
							});
						});
					}
					// @TODO: Reflected Entries

					return {std::move(out)};
				}
			}(),
		});

		const usize index = impl::type_infos.size() - 1;
		ASSERT(index < UINT16_MAX);

		return TypeId{static_cast<u16>(index)};
	}();

	return ID;
}

[[nodiscard]] FORCEINLINE auto get_type_info(const TypeId id) -> const TypeInfo& {
	return impl::type_infos[id];
}

template <typename T>
[[nodiscard]] FORCEINLINE auto get_type_info() -> const TypeInfo& {
	return get_type_info(get_type_id<T>());
}
}

// Uses rtti to represent any type at runtime.
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
		ASSERTF(self.template is<T>(), "Could not cast {} to {}!", rtti::get_type_info(self.type).name, utils::get_type_name<T>());
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
#endif
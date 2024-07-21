#pragma once

#include "core_include.hpp"
#include "defines.hpp"
#include "fmt/printf.h"
#include "types.hpp"
#include "traits.hpp"
#include "utils.hpp"
#include "optional.hpp"

#if 0
enum class MemberFlags : u8 {
	NONE = 0,
	SERIALIZABLE = 1 << 0,
	DEFAULT = SERIALIZABLE,
};
DECLARE_ENUM_CLASS_FLAGS(MemberFlags);

template <usize N, typename T, typename M>
requires std::is_class_v<T>
struct Member {
	consteval explicit Member(const char (&name)[N], const M T::* member, const MemberFlags flags = MemberFlags::DEFAULT)
		: name{name}, member{member}, flags{flags} {}

	StringLiteral<N> name;
	const M T::* member;
	MemberFlags flags;
	using MemberType = std::decay_t<M>;
};

template <usize N, typename E>
requires std::is_enum_v<E>
struct Entry {
	consteval explicit Entry(const char (&name)[N], const E value)
		: name{name}, value{static_cast<__underlying_type(E)>(value)} {}

	StringLiteral<N> name;
	u64 value;
};

namespace cpts {
template <typename T>
concept Member = requires(T t) {// Not entirely accurate but good enough probably.
	t.member;
};
}

template <auto... IN_MEMBERS>
requires (cpts::Member<std::decay_t<decltype(IN_MEMBERS)>> && ...)
struct ReflectMembers {
	static constexpr Tuple<decltype(IN_MEMBERS)...> MEMBERS = std::tie(IN_MEMBERS...);
	using Members = Tuple<typename std::decay_t<decltype(IN_MEMBERS)>::MemberType...>;

	template <usize N>
	static consteval auto find_member_by_name(const char (&name)[N]) {
		return find_member_by_name(StringLiteral{name});
	}

#if 0
	template <usize N>
	static consteval auto find_member_by_name() {
		return [&]<usize I>(this const auto& self) {
			#if 0
			if constexpr (I >= sizeof...(IN_MEMBERS)) {
				static_assert(false, "Could not find member in list!");
			}
			
			static constexpr const auto& MEMBER = std::get<I>(MEMBERS);
			if constexpr (MEMBER.name == StringLiteral{name}) {
				return MEMBER;
			} else {
				return self.template operator()<I + 1>();
			}
			#endif

			static constexpr const auto& MEMBER = std::get<I>(MEMBERS);
			if constexpr (StringLiteral{"Bill"} == name) {

			}

			return MEMBER;
		}.template operator()<0>();
	}
#endif

	template <StringLiteral NAME>
	static consteval auto find_member_by_name() {
		return [&]<usize I>(this const auto& self) consteval {
			if constexpr (I >= sizeof...(IN_MEMBERS)) {
				static_assert(false, "Could not find member in list!");
			}

			static constexpr auto& MEMBER = std::get<I>(MEMBERS);
			if constexpr (MEMBER.name == NAME) {
				return MEMBER;
			} else {
				return self.template operator()<I + 1>();
			}
		}.template operator()<0>();
	}
};

template <auto... IN_ENTRIES>
struct ReflectEntries {
	static constexpr auto ENTRIES = std::tie(IN_ENTRIES...);
};

template <typename... Ps>
requires (std::is_class_v<Ps> && ...)
struct ReflectParents {
	using Parents = Tuple<Ps...>;
};

template <typename>
struct ReflectionTrait;

namespace cpts {
template <typename T>
concept ReflectedMembers = requires {
	ReflectionTrait<T>::Members;
};

template <typename T>
concept ReflectedEntries = requires {
	ReflectionTrait<T>::Entries;
};

template <typename T>
concept ReflectedParent = requires {
	ReflectionTrait<T>::Parent;
};

template <typename T>
concept Reflected = ReflectedMembers<T> || ReflectedParent<T>;
}

#if 0
struct TypeInfo;

struct TypeId : public IntAlias<u32> {
	using IntAlias<u32>::IntAlias;

	[[nodiscard]] auto is_a(const TypeId other) const -> bool;

	template <typename T>
	[[nodiscard]] auto is_a() const -> bool;
};

enum class StructureType : u8 {
	Primitive, Struct, Enum,
};

enum class PrimitiveType : u8 {
	BOOL, I8, U8, I16, U16, I32, U32, I64, U64, F32, F64,
};

enum class MemberFlags : u8 {
	NONE = 0,
	SERIALIZABLE = 1 << 0,

	DEFAULT = MemberFlags::SERIALIZABLE,
};
DECLARE_ENUM_CLASS_FLAGS(MemberFlags);

// @TODO: Potentially reflect member functions (although who needs those anyways).
struct ReflectionInfo {
	struct Member {
		[[nodiscard]] FORCEINLINE auto get_value_in_container(void* container) const -> void* {
			return static_cast<u8*>(container) + offset;
		}

		[[nodiscard]] FORCEINLINE auto get_value_in_container(const void* container) const -> const void* {
			return static_cast<const u8*>(container) + offset;
		}

		String name;
		usize offset;
		TypeId type_id;
		MemberFlags flags;
	};

	template <Invokable<const Member&> Func>
	auto for_each_member(Func&& fn) const -> void;

	template <typename T>
	requires (std::is_class_v<T>)
	auto child_of() -> ReflectionInfo&;

	template <typename T, typename MemberType>
	requires (std::is_class_v<T>)
	auto add_member(String name, const MemberType T::* member, const MemberFlags flags = MemberFlags::DEFAULT) -> ReflectionInfo&;

	Optional<TypeId> parent;
	Array<Member> members;
};

struct EnumReflectionInfo {
	struct Entry {
		StringView name;
		u64 value;
	};

	Array<Entry> entries;
};

// Only supports structs for now.
struct TypeInfo {
	String name;
	usize size, alignment;
	void(*construct)(void*, usize);
	void(*copy_construct)(void*, const void*, usize);
	void(*move_construct)(void*, void*, usize);
	void(*copy_assign)(void*, const void*, usize);
	void(*move_assign)(void*, void*, usize);
	void(*destruct)(void*, usize);
	String(*to_string)(const void*);
	void(*from_string)(void*, StringView);
	Optional<ReflectionInfo> reflection_info;
	u8 is_trivially_constructible: 1;
	u8 is_trivially_copy_constructible: 1;
	u8 is_trivially_destructible: 1;
	u8 has_to_string: 1;
	u8 has_from_string: 1;
};

template <typename>
struct ReflectionTrait;

namespace cpts {
template <typename T>
concept Reflected = requires(ReflectionInfo r) {
	{ ReflectionTrait<T>::reflect(r) };
};
}

template <typename T>
auto get_type_name() -> const String& {
	static const String NAME = [] {
		if constexpr (requires { { ReflectionTrait<T>::name() } -> std::same_as<String>; }) {
			return ReflectionTrait<T>::name();// Not sure how useful this is.
		} else {
			static constexpr StringView TYPE_NAME_REDIRECTS[][2] = {
				{ "std::vector", "Array", },
				{ "std::span", "Span", },
				{ "std::array", "StaticArray", },
				{ "std::unordered_set", "Set", },
				{ "std::unordered_map", "Map", },
				{ "std::optional", "Optional", },
				{ "std::function", "Fn", },
				{ "std::tuple", "Tuple", },
				{ "std::pair", "Pair", },
				{ "std::variant", "Variant", },
				{ "std::shared_ptr", "SharedPtr", },
				{ "std::weak_ptr", "WeakPtr", },
				{ "std::unique_ptr", "UniquePtr", },
				{ "std::basic_string_view<char>", "StringView", },
				{ "std::basic_string_view", "StringView", },
				{ "std::basic_string<char>", "String", },
				{ "std::basic_string", "String", },
				{ "unsigned char", "u8", },
				{ "unsigned short", "u16", },
				{ "unsigned int", "u32", },
				{ "unsigned long long", "u64", },
				{ "unsigned long", "u32", },
				{ "char", "i8" },
				{ "short", "i16" },
				{ "int", "i32", },
				{ "long long", "i64", },
				{ "long", "i32", },
			};

			// Don't like the space added to reference types.
			static constexpr StringView QUALIFIER_REDIRECTS[][2] = {
				{ " &", "&", },
				{ " *", "*", },
			};

			String name{utils::get_raw_type_name<T>()};
			for (const auto& [symbol, redirect] : TYPE_NAME_REDIRECTS) {
				usize pos = 0;
				while (true) {
					pos = name.find(symbol, pos);

					// A type should always be surrounded by one of these characters on either end.
					static constexpr StringView CHARS_SURROUNDING_TYPE = " <>:,*&";
					if (pos == StringView::npos) {
						break;
					} else if ((pos != 0 && !CHARS_SURROUNDING_TYPE.contains(name.at(pos - 1))) ||
						(pos + symbol.size() != name.size() && !CHARS_SURROUNDING_TYPE.contains(name.at(pos + symbol.size())))) {
						pos += symbol.size();// Skip this symbol as it wasn't actually a type.
						continue;
					}

					name.replace(pos, symbol.size(), redirect);
				}
			}

			for (const auto& [symbol, redirect] : QUALIFIER_REDIRECTS) {
				usize pos = 0;
				while (true) {
					pos = name.find(symbol, pos);
					if (pos == StringView::npos) {
						break;
					}

					name.replace(pos, symbol.size(), redirect);
				}
			}

			return name;
		}
	}();

	return NAME;
}

namespace impl {
EXPORT_API inline Array<TypeInfo> type_infos;

template <typename T>
struct TypeIdInstantiator {
	EXPORT_API inline static auto ID = []() -> TypeId {
		const auto index = type_infos.size();
		ASSERT(index < UINT32_MAX);

		type_infos.push_back(TypeInfo{
			.name = get_type_name<T>(),
			.size = sizeof(T),
			.alignment = alignof(T),
			.construct = [](void* dst, const usize count) {
				if constexpr (std::is_trivially_constructible_v<T>) {
					memset(dst, 0, count * sizeof(T));
				} else {
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
			.to_string = [](const void* src) -> String {
				if constexpr (cpts::HasToString<T>) {
					return to_string(*static_cast<const T*>(src));
				} else {
					UNIMPLEMENTED;
				}
			},
			.from_string = [](void* dst, const StringView string) {
				if constexpr (cpts::HasFromString<T>) {
					from_string(*static_cast<T*>(dst), string);
				} else {
					UNIMPLEMENTED;
				}
			},
			.reflection_info = []() -> Optional<ReflectionInfo> {
				if constexpr (cpts::Reflected<T>) {
					ReflectionInfo out;
					ReflectionTrait<T>::reflect(out);
					ASSERTF(!out.members.empty() || out.parent.has_value(), "Implemented reflection trait on {} but reflected nothing!", utils::get_raw_type_name<T>());

					std::sort(out.members.begin(), out.members.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });

					return out;
				} else {
					return {};
				}
			}(),
			.is_trivially_constructible = std::is_trivially_constructible_v<T>,
			.is_trivially_copy_constructible = std::is_trivially_copy_constructible_v<T>,
			.is_trivially_destructible = std::is_trivially_destructible_v<T>,
			.has_to_string = cpts::HasToString<T>,
			.has_from_string = cpts::HasFromString<T>,
		});

		return TypeId{static_cast<u32>(index)};
	}();
};
}

template <typename T>
[[nodiscard]] FORCEINLINE auto get_type_id() -> TypeId {
	return impl::TypeIdInstantiator<T>::ID;
}

[[nodiscard]] FORCEINLINE auto get_type_info(const TypeId type_id) -> const TypeInfo& {
	return impl::type_infos[type_id];
}

template <typename T>
[[nodiscard]] FORCEINLINE auto get_type_info() -> const TypeInfo& {
	return get_type_info(get_type_id<T>());
}

[[nodiscard]] inline auto TypeId::is_a(TypeId other) const -> bool {
	while (true) {
		if (other == *this) {
			return true;
		}

		const auto& type_info = get_type_info(other);
		if (!type_info.reflection_info || !type_info.reflection_info->parent) {
			break;
		}

		other = *type_info.reflection_info->parent;
	}

	return false;
}

template <typename T>
[[nodiscard]] FORCEINLINE auto TypeId::is_a() const -> bool {
	return is_a(get_type_id<T>());
}

template <Invokable<const ReflectionInfo::Member&> Func>
inline auto ReflectionInfo::for_each_member(Func&& fn) const -> void {
	// @NOTE: This accesses members from child -> parent which may technically be less cache efficient than the other way around.
	const auto exec = [&fn](const ReflectionInfo& info) {
		for (const Member& member : info.members) {
			std::invoke(fn, member);
		}
	};

	exec(*this);

	if (parent) {
		Optional<TypeId> next = parent;
		do {
			const auto& next_type_info = get_type_info(*next);
			if (!next_type_info.reflection_info) {
				break;
			}

			exec(*next_type_info.reflection_info);

			next = next_type_info.reflection_info->parent;
		} while (next);
	}
}

template <typename T>
requires (std::is_class_v<T>)
inline auto ReflectionInfo::child_of() -> ReflectionInfo& {
	ASSERTF(!parent.has_value(), "Reflection does not support multiple inheritance!");
	parent = get_type_id<T>();
	return *this;
}

template <typename T, typename MemberType>
requires (std::is_class_v<T>)
inline auto ReflectionInfo::add_member(String name, const MemberType T::* member, const MemberFlags flags) -> ReflectionInfo& {
	members.push_back(Member{
		.name = std::move(name),
		.offset = utils::get_member_offset<T, MemberType>(member),
		.type_id = get_type_id<std::decay_t<MemberType>>(),
		.flags = flags,
	});
	return *this;
}
#endif
#endif

#if 0
template <typename>
struct ReflectionTrait;

namespace cpts {
template <typename T>
concept ReflectedMembers = requires {
	typename ReflectionTrait<T>::Members;
};

template <typename T>
concept ReflectedParents = requires {
	typename ReflectionTrait<T>::Parents;
};

template <typename T>
concept Reflected = ReflectedMembers<T> || ReflectedParents<T>;
}

enum class MemberFlags : u8 {
	NONE = 0,
	SERIALIZABLE = 1 << 0,

	DEFAULT = SERIALIZABLE,
};
DECLARE_ENUM_CLASS_FLAGS(MemberFlags);

template <StringLiteral IN_NAME, MemberPtr IN_MEMBER, MemberFlags IN_FLAGS = MemberFlags::DEFAULT>
struct Member {
	static constexpr auto NAME = IN_NAME;
	static constexpr auto MEMBER = IN_MEMBER;
	static constexpr auto FLAGS = IN_FLAGS;

	using MemberType = typename std::decay_t<decltype(MEMBER)>::Member;
	using ContainerType = typename std::decay_t<decltype(MEMBER)>::Container;

	static constexpr auto get_value_in_container(ContainerType& container) -> MemberType& {
		return container.*MEMBER.member;
	}

	static constexpr auto get_value_in_container(const ContainerType& container) -> const MemberType& {
		return container.*MEMBER.member;
	}
};

namespace cpts {
template <typename T>
concept Member = requires {
	T::NAME;
	T::MEMBER;
	{ T::FLAGS } -> std::convertible_to<MemberFlags>;
	typename T::MemberType;
	typename T::ContainerType;
};
}

template <typename... InMembers>
struct Members {
	using MemberTypes = Tuple<InMembers...>;
	static constexpr usize COUNT = sizeof...(InMembers);

	static constexpr auto for_each_member(auto&& fn, auto&&... args) -> void {
		(fn.template operator()<InMembers>(FORWARD_AUTO(args)...), ...);
	}

	template <StringLiteral NAME>
	static consteval auto has_member_by_name() -> bool {
		return ((InMembers::NAME == NAME) || ...);
	}

private:
	template <StringLiteral NAME, typename T, typename... Ts>
	struct InternalFindMemberByName {
		using Type = typename InternalFindMemberByName<NAME, Ts...>::Type;
	};

	template <StringLiteral NAME, typename T, typename... Ts>
	requires (T::NAME == NAME)
	struct InternalFindMemberByName<NAME, T, Ts...> {
		using Type = T;
	};

public:
	template <StringLiteral NAME>
	requires (has_member_by_name<NAME>())
	using FindMemberByName = typename InternalFindMemberByName<NAME, InMembers...>::Type;
};

template <typename... InParents>
struct Parents {
	using ParentTypes = Tuple<InParents...>;
	static constexpr usize COUNT = sizeof...(InParents);

	static constexpr auto for_each_parent(auto&& fn, auto&&... args) -> void {
		(fn.template operator()<InParents>(FORWARD_AUTO(args)...), ...);
	}
};
#endif

#if 0
// Metadata holds a name and an optional value associated.
template <StringLiteral IN_NAME, auto IN_VALUE = nullptr>
struct Meta {
	static constexpr auto NAME = IN_NAME;
	static constexpr auto VALUE = IN_VALUE;
	using ValueType = std::decay_t<decltype(IN_VALUE)>;
	static constexpr bool HAS_VALUE = std::is_same_v<ValueType, std::nullptr_t>;
};

namespace cpts {
template <typename T>
concept Meta = requires {
	T::NAME;
	T::VALUE;
	T::HAS_VALUE;
	typename T::ValueType;
};
}
#endif

template <usize N, typename T = std::nullptr_t>
struct Attribute {
	consteval Attribute(const char (&name)[N], T&& value = null)
		: name{name}, value{std::forward<T>(value)} {}

	using ValueType = T;
	static constexpr bool HAS_VALUE = !cpts::Same<T, std::nullptr_t>;

	StringLiteral<N> name;
	T value;
	bool has_value = HAS_VALUE;
};

template <StringLiteral IN_NAME, MemberPtr IN_MEMBER, Attribute... IN_ATTRIBUTES>
struct Member {
	static constexpr StringLiteral NAME = IN_NAME;
	static constexpr MemberPtr MEMBER = IN_MEMBER;
	static constexpr auto ATTRIBUTES = utils::make_tuple(IN_ATTRIBUTES...);
	using MemberType = typename std::decay_t<decltype(IN_MEMBER)>::Member;
	using ContainerType = typename std::decay_t<decltype(IN_MEMBER)>::Container;

	template <StringLiteral ATTRIBUTE_NAME>
	static consteval auto has_attribute_by_name() -> bool {
		return ((IN_ATTRIBUTES.name == ATTRIBUTE_NAME) || ...);
	}

	// Returns an optional depending on the type exists or not. If there's no value the optional contains std::nullptr_t.
	template <StringLiteral ATTRIBUTE_NAME>
	requires (has_attribute_by_name<ATTRIBUTE_NAME>())
	static consteval auto get_attribute_value_by_name() {
		return []<usize I>(this auto&& self) consteval {
			const auto& metadata = ATTRIBUTES.template get<I>();
			if constexpr (metadata.name == ATTRIBUTE_NAME) {
				return metadata;
			} else {
				return self.template operator()<I + 1>();
			}
		}.template operator()<0>();
	}

	// Will return an optional. An Optional<std::nullptr_t> will be returned if this value does not exist.
	template <StringLiteral ATTRIBUTE_NAME>
	static consteval auto try_get_attribute_value_by_name() {
		if constexpr (has_attribute_by_name<ATTRIBUTE_NAME>()) {
			static constexpr auto VALUE = get_attribute_value_by_name<ATTRIBUTE_NAME>();
			return Optional<std::decay_t<decltype(VALUE)>>{VALUE};
		} else {
			return Optional<std::nullptr_t>{};
		}
	}

	[[nodiscard]] FORCEINLINE static constexpr auto get_value_in_container(auto&& container) -> decltype(auto) {
		return IN_MEMBER.get_value_in_container(FORWARD_AUTO(container));
	}
};

namespace cpts {
template <typename T>
concept Member = requires {
	T::NAME;
	T::ATTRIBUTES;
	typename T::MemberType;
	typename T::ContainerType;
};
}

template <cpts::Member... InMembers>
struct Members {
private:
	template <StringLiteral NAME, typename T, typename... Ts>
	struct InternalFindMemberByName {
		using Type = typename InternalFindMemberByName<NAME, Ts...>::Type;
	};

	template <StringLiteral NAME, typename T, typename... Ts>
	requires (T::NAME == NAME)
	struct InternalFindMemberByName<NAME, T, Ts...> {
		using Type = T;
	};

	template <MemberPtr MEMBER, typename T, typename... Ts>
	struct InternalFindMemberByPtr {
		using Type = typename InternalFindMemberByPtr<MEMBER, Ts...>::Type;
	};

	template <MemberPtr MEMBER, typename T, typename... Ts>
	requires (T::MEMBER == MEMBER)
	struct InternalFindMemberByPtr<MEMBER, T, Ts...> {
		using Type = T;
	};

public:
	using MemberTypes = TypeList<InMembers...>;
	static constexpr usize COUNT = sizeof...(InMembers);

	template <StringLiteral NAME>
	static consteval auto contains_member_by_name() -> bool {
		return ((InMembers::NAME == NAME) && ...);
	}

	template <StringLiteral NAME>
	static consteval auto index_of_member_by_name() -> usize {
		return utils::index_of_type_by_predicate<InMembers...>([]<typename T>() consteval -> bool {
			return T::NAME == NAME;
		});
	}

	template <MemberPtr MEMBER>
	static consteval auto index_of_member() -> usize {
		return utils::index_of_type_by_predicate<InMembers...>([]<typename T>() consteval -> bool {
			return T::MEMBER == MEMBER;
		});
	}

	template <StringLiteral NAME>
	using FindMemberByName = typename InternalFindMemberByName<NAME, InMembers...>::Type;

	template <MemberPtr MEMBER>
	using FindMember = typename InternalFindMemberByPtr<MEMBER, InMembers...>::Type;

	FORCEINLINE static constexpr auto for_each(auto&& fn, auto&&... args) -> void {
		((fn.template operator()<InMembers>(FORWARD_AUTO(args)...)), ...);
	}

	FORCEINLINE static constexpr auto make_param_pack(auto&& fn, auto&&... args) -> decltype(auto) {
		return (fn.template operator()<InMembers...>(FORWARD_AUTO(args)...));
	}
};

template <Attribute... IN_ATTRIBUTES>
struct Attributes {
	static constexpr auto ATTRIBUTES = make_tuple(IN_ATTRIBUTES...);

	// Returns an optional depending on the type exists or not. If there's no value the optional contains std::nullptr_t.
	template <StringLiteral ATTRIBUTE_NAME>
	requires (has_attribute_by_name<ATTRIBUTE_NAME>())
	static consteval auto get_attribute_value_by_name() {
		return []<usize I>(this auto&& self) consteval {
			const auto& metadata = ATTRIBUTES.template get<I>();
			if constexpr (metadata.name == ATTRIBUTE_NAME) {
				return metadata;
			} else {
				return self.template operator()<I + 1>();
			}
		}.template operator()<0>();
	}

	// Will return an optional. An Optional<std::nullptr_t> will be returned if this value does not exist.
	template <StringLiteral ATTRIBUTE_NAME>
	static consteval auto try_get_attribute_value_by_name() {
		if constexpr (has_attribute_by_name<ATTRIBUTE_NAME>()) {
			static constexpr auto VALUE = get_attribute_value_by_name<ATTRIBUTE_NAME>();
			return Optional<std::decay_t<decltype(VALUE)>>{VALUE};
		} else {
			return Optional<std::nullptr_t>{};
		}
	}
};

template <typename>
struct Reflect;

namespace cpts {
template <typename T>
concept ReflectedMembers = requires {
	typename Reflect<T>::Members;
};

template <typename T>
concept ReflectedParent = requires {
	typename Reflect<T>::Parent;
};

template <typename T>
concept ReflectedAttributes = requires {
	typename Reflect<T>::Attributes;
};

template <typename T>
concept Reflected = ReflectedMembers<T> || ReflectedParent<T>;
}

namespace utils {
template <StringLiteral MEMBER>
[[nodiscard]] consteval auto get_name_from_member_ptr() {
	static constexpr auto BEGIN = MEMBER.view().rfind(':') + 1;
	static constexpr auto COUNT = std::decay_t<decltype(MEMBER)>::COUNT - BEGIN;

	StringLiteral<COUNT> out{NO_INIT};
	std::copy_n(&MEMBER.value[BEGIN], COUNT, out.value);
	return out;
}
}

namespace attributes {
constexpr const char NOT_SERIALIZED[] = "NotSerialized";
}

namespace meta {
namespace impl {
template <typename T, typename... Members>
struct MembersAccumulator {
	using Type = TypeList<Members...>;
};

template <typename T, typename... Members> requires (cpts::ReflectedMembers<T>)
struct MembersAccumulator<T, Members...> {
	using Type = typename Reflect<T>::Members::MemberTypes;
};

template <typename T, typename... Members> requires (cpts::ReflectedParent<T>)
struct MembersAccumulator<T, Members...> {
	using Type = typename MembersAccumulator<typename Reflect<T>::Parent, Members...>::Type;
};

template <cpts::ReflectedMembers T, typename... Members> requires (cpts::ReflectedMembers<T> && cpts::ReflectedParent<T>)
struct MembersAccumulator<T, Members...> {
	using Type = utils::Concat<
		typename MembersAccumulator<typename Reflect<T>::Parent, Members...>::Type,
		typename Reflect<T>::Members::MemberTypes
	>;
};
}

// Gathers all reflected members - includes parents.
template <cpts::Reflected T>
FORCEINLINE constexpr auto make_reflected_members_param_pack(auto&& fn, auto&&... params) -> decltype(auto) {
	return (utils::make_param_pack<typename impl::MembersAccumulator<T>::Type>(FORWARD_AUTO(fn), FORWARD_AUTO(params)...));
}
}

#define DECLARE_MEMBER(member, ...) Member<utils::get_name_from_member_ptr<#member>(), member, ##__VA_ARGS__>

// Default reflected types.
template <typename T1, typename T2>
struct Reflect<Pair<T1, T2>> {
	using Members = Members<
		Member<"first", &Pair<T1, T2>::first>,
		Member<"second", &Pair<T1, T2>::second>
	>;
};
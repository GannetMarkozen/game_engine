#pragma once

#include "defines.hpp"
#include <concepts>
#include <type_traits>
#include <utility>

template <typename T, typename Allocator = std::allocator<T>>
using Array = std::vector<T, Allocator>;

template <typename T, usize EXTENT = std::dynamic_extent>
using Span = std::span<T, EXTENT>;

template <typename T, usize SIZE>
using StaticArray = std::array<T, SIZE>;

template <typename T1, typename T2>
using Pair = std::pair<T1, T2>;

using String = std::string;
using StringView = std::string_view;

template <typename... Ts>
using Variant = std::variant<Ts...>;

template <typename Key, typename Value, typename HashOp = std::hash<Key>, typename EqualOp = std::equal_to<Key>, typename Allocator = std::allocator<Pair<const Key, Value>>>
using Map = std::unordered_map<Key, Value, HashOp, EqualOp, Allocator>;

template <typename T, typename HashOp = std::hash<T>, typename EqualOp = std::equal_to<T>, typename Allocator = std::allocator<T>>
using Set = std::unordered_set<T, HashOp, EqualOp, Allocator>;

template <typename T, typename Deleter = std::default_delete<T>>
using UniquePtr = std::unique_ptr<T, Deleter>;

template <typename T>
using SharedPtr = std::shared_ptr<T>;

template <typename T>
using WeakPtr = std::weak_ptr<T>;

template <typename>
struct Fn;

template <typename Return, typename... Params>
struct Fn<Return(Params...)> final : public std::function<Return(Params...)> {
	using std::function<Return(Params...)>::function;
};

template <typename>
struct FnRef;

template <typename Return, typename... Params>
struct FnRef<Return(Params...)> {
	constexpr FnRef(const FnRef&) = default;
	constexpr auto operator=(const FnRef&) -> FnRef& = default;

	template <cpts::Invokable<Return, Params...> T>
	FORCEINLINE constexpr FnRef(T&& other [[clang::lifetimebound]])
		:
		data{&other},
		fn{[](void* data, Params&&... params) -> Return {
			return std::invoke(static_cast<std::decay_t<T>*>(data), std::forward<Params>(params)...);
		}} {}

	FORCEINLINE constexpr auto operator()(Params... params) const -> Return {
		return fn(data, std::forward<Params>(params)...);
	}

private:
	void* data;
	Return(*fn)(void*, Params...);
};

enum NullOptional {
	NULL_OPTIONAL
};

template <typename T>
struct Optional : public std::optional<T> {
	using Super = std::optional<T>;
	using Super::Super;

	constexpr Optional(NullOptional)
		: Super{} {}
};

namespace cpts {
template <typename T>
concept Hashable = requires (const T t) {
	{ std::hash<T>{}(t) } -> std::same_as<usize>;
};
}

namespace utils {
template <typename... Ts>
[[nodiscard]] consteval auto contains_type_by_predicate(auto&& fn) -> bool {
	return (fn.template operator()<Ts>() || ...);
}

template <typename... Ts>
[[nodiscard]] consteval auto index_of_type_by_predicate(auto&& fn) -> usize {
	usize index = 0;
	((fn.template operator()<Ts>() ? true : [&] { ++index; return false; }()) || ...);
	return index;
}

template <typename T, typename... Ts>
[[nodiscard]] consteval auto contains_type() -> bool {
	return (std::is_same_v<T, Ts> || ...);
}

template <typename T, typename... Ts>
requires (contains_type<T, Ts...>())
[[nodiscard]] consteval auto index_of_type() -> usize {
	usize index = 0;
	((std::is_same_v<T, Ts> ? true : [&] { ++index; return false; }()) || ...);
	return index;
}

template <typename T, typename Member>
[[nodiscard]] auto get_member_offset(const Member T::* member) -> usize {
	return reinterpret_cast<usize>(&(static_cast<T*>(null)->*member));
}
}

namespace utils {
namespace impl {
template <usize I, typename T, typename... Ts>
struct TypeAtIndex {
	static_assert(sizeof...(Ts) > 0, "Could not find type at index!");
	using Type = typename TypeAtIndex<I - 1, Ts...>::Type;
};

template <typename T, typename... Ts>
struct TypeAtIndex<0, T, Ts...> {
	using Type = T;
};

template <usize, typename>
struct TypeAtIndexInContainer;

template <usize I, template <typename...> typename Container, typename... Ts>
struct TypeAtIndexInContainer<I, Container<Ts...>> {
	using Type = typename TypeAtIndex<I, Ts...>::Type;
};
}

template <usize I, typename... Ts>
using TypeAtIndex = typename impl::TypeAtIndex<I, Ts...>::Type;

template <usize I, typename Container>
using TypeAtIndexInContainer = typename impl::TypeAtIndexInContainer<I, Container>::Type;
}

template <typename... Ts>
using Tuple = std::tuple<Ts...>;

namespace cpts {
namespace impl {
template <typename>
struct IsTuple;

template <template <typename...> typename Container, typename... Ts>
struct IsTuple<Container<Ts...>> {
	static constexpr bool VALUE = std::same_as<Container<Ts...>, Tuple<Ts...>>;
};
}

template <typename T>
concept IsTuple = impl::IsTuple<T>::VALUE;
}

namespace utils {
template <typename... Args>
[[nodiscard]] FORCEINLINE constexpr auto make_tuple(Args&&... args) {
	return Tuple<std::decay_t<Args>...>{std::forward<Args>(args)...};
}

template <typename... Args>
[[nodiscard]] FORCEINLINE constexpr auto tie(Args&... outs) {
	return Tuple<Args&...>{outs...};
}
}

template <typename... Ts>
struct TypeList {};

#define _DECLARE_INT_ALIAS(NAME, TYPE, ...) \
	static_assert(std::integral<TYPE>, "Declared int alias " #NAME " that doesn't have an integer type!"); \
	struct NAME { \
		using Type = TYPE; \
		\
		constexpr NAME(const NAME&) = default; \
		constexpr NAME(NAME&&) noexcept = default; \
		constexpr auto operator=(const NAME&) -> NAME& = default; \
		constexpr auto operator=(NAME&&) noexcept -> NAME& = default; \
		\
		constexpr NAME() : value{0} {} \
		constexpr explicit NAME(NoInit) {} \
		\
		template <std::convertible_to<TYPE> T> requires (!::cpts::IntAlias<std::decay_t<T>>) \
		constexpr NAME(T&& in_value) : value{static_cast<TYPE>(std::forward<T>(in_value))} {} \
		\
		template <std::convertible_to<TYPE> T> requires (!::cpts::IntAlias<std::decay_t<T>>) \
		FORCEINLINE constexpr auto operator=(T&& in_value) -> NAME& { \
			value = static_cast<TYPE>(std::forward<T>(in_value)); \
			return *this; \
		} \
		\
		[[nodiscard]] FORCEINLINE constexpr auto operator==(const NAME other) const -> bool { \
			return value == other.value; \
		} \
		\
		[[nodiscard]] FORCEINLINE constexpr auto operator!=(const NAME other) const -> bool { \
			return value != other.value; \
		} \
		\
		[[nodiscard]] FORCEINLINE constexpr auto operator<=>(const NAME other) const { \
			return value <=> other.value; \
		} \
		\
		template <std::three_way_comparable_with<TYPE> T> requires (!std::same_as<std::decay_t<T>, NAME>) \
		[[nodiscard]] FORCEINLINE constexpr auto operator<=>(const T& other) const { \
			return value <=> other; \
		} \
		\
		FORCEINLINE constexpr auto operator+=(const std::integral auto other) -> NAME& { \
			value += other; \
			return *this; \
		} \
		\
		FORCEINLINE constexpr auto operator-=(const std::integral auto other) -> NAME& { \
			value -= other; \
			return *this; \
		} \
		\
		FORCEINLINE constexpr auto operator*=(const std::integral auto other) -> NAME& { \
			value *= other; \
			return *this; \
		} \
		\
		FORCEINLINE constexpr auto operator/=(const std::integral auto other) -> NAME& { \
			value /= other; \
			return *this; \
		} \
		\
		FORCEINLINE constexpr auto operator++() -> NAME& { \
			++value; \
			return *this; \
		} \
		\
		FORCEINLINE constexpr auto operator++(int) -> NAME { \
			auto old = *this; \
			++*this; \
			return old; \
		} \
		\
		FORCEINLINE constexpr auto operator--() -> NAME& { \
			--value; \
			return *this; \
		} \
		\
		FORCEINLINE constexpr auto operator--(int) -> NAME { \
			auto old = *this; \
			--*this; \
			return old; \
		} \
		\
		template <typename T> requires (std::is_arithmetic_v<T> && std::convertible_to<TYPE, T>) \
		[[nodiscard]] FORCEINLINE constexpr operator T() const { return static_cast<T>(value); } \
		\
		[[nodiscard]] FORCEINLINE constexpr operator TYPE&() { return value; } \
		[[nodiscard]] FORCEINLINE constexpr operator const TYPE&() const { return value; } \
		\
		[[nodiscard]] FORCEINLINE constexpr auto get_value() const -> TYPE { return value; } \
		FORCEINLINE constexpr auto set_value(const TYPE new_value) -> NAME& { value = new_value; return *this; } \
		\
		[[nodiscard]] static consteval auto max() -> NAME { \
			return NAME{std::numeric_limits<TYPE>::max()}; \
		} \
		\
		[[nodiscard]] static consteval auto min() -> NAME { \
			return NAME{std::numeric_limits<TYPE>::min()}; \
		} \
		\
		[[nodiscard]] FORCEINLINE static constexpr auto invalid_id() -> NAME { \
			if constexpr (std::signed_integral<TYPE>) { \
				return min(); \
			} else { \
				return max(); \
			} \
		} \
		\
		[[nodiscard]] FORCEINLINE constexpr auto is_valid() const -> bool { \
			return *this != invalid_id(); \
		} \
		__VA_ARGS__ \
	private: \
		TYPE value; \
	};

// Must be declared in global scope.
#define DECLARE_INT_ALIAS(NAME, TYPE, ...) \
	_DECLARE_INT_ALIAS(NAME, TYPE, __VA_ARGS__) \
	\
	template <> \
	struct std::hash<NAME> { \
		[[nodiscard]] FORCEINLINE constexpr auto operator()(const NAME value) const -> usize { \
			return std::hash<TYPE>{}(value.get_value()); \
		} \
	};

// Must be declared in global scope.
#define DECLARE_NAMESPACED_INT_ALIAS(NAMESPACE, NAME, TYPE, ...) \
	namespace NAMESPACE { \
		_DECLARE_INT_ALIAS(NAME, TYPE, __VA_ARGS__) \
	} \
	\
	template <> \
	struct std::hash<NAMESPACE::NAME> { \
		[[nodiscard]] FORCEINLINE constexpr auto operator()(const NAMESPACE::NAME value) const -> usize { \
			return std::hash<TYPE>{}(value.get_value()); \
		} \
	};

namespace cpts {
template <typename T>
concept IntAlias = requires {
	typename T::Type;
} && !std::integral<T> && std::integral<typename T::Type> && sizeof(T) == sizeof(typename T::Type) && alignof(T) == alignof(typename T::Type);
}

template <usize N>
struct StringLiteral {
	static constexpr usize COUNT = N;

	consteval StringLiteral(NoInit) {}

	consteval StringLiteral(const char (&str)[N]) {
		std::copy_n(str, N, value);
	}

	FORCEINLINE constexpr StringLiteral(const StringLiteral& other) {
		std::copy_n(other.value, N, value);
	}

	FORCEINLINE constexpr auto operator=(const StringLiteral& other) -> StringLiteral& {
		return *new(this) StringLiteral{other};
	}

	template <usize OtherN>
	[[nodiscard]] FORCEINLINE constexpr auto operator==(const StringLiteral<OtherN>& other) const -> bool {
		if constexpr (N != OtherN) {
			return false;
		}

		if consteval {
			for (usize i = 0; i < N; ++i) {
				if (value[i] != other.value[i]) {
					return false;
				}
			}
			return true;
		} else {
			return std::strcmp(value, other.value) == 0;
		}
	}

	[[nodiscard]] FORCEINLINE constexpr auto view() const -> StringView {
		return StringView{value, N};
	}

	[[nodiscard]] FORCEINLINE constexpr operator StringView() const {
		return view();
	}

	[[nodiscard]] consteval auto size() const -> usize {
		return N;
	}

	char value[N];
};

template <typename T, typename M>
struct MemberPtr {
	template <typename, typename>
	friend struct MemberPtr;

	constexpr explicit MemberPtr(NoInit) {}

	consteval MemberPtr(const M T::* member)
		: member{member} {}

	consteval MemberPtr(const MemberPtr&) = default;
	consteval auto operator=(const MemberPtr&) -> MemberPtr& = default;

	const M T::* member;

	using Member = std::decay_t<M>;
	using Container = std::decay_t<T>;

	template <typename OtherT, typename OtherM>
	[[nodiscard]] FORCEINLINE constexpr auto operator==(const MemberPtr<OtherT, OtherM>& other) const -> bool {
		if constexpr (!std::same_as<T, OtherT> || !std::same_as<M, OtherM>) {
			return false;
		} else {
			return member == other.member;
		}
	}

	[[nodiscard]] FORCEINLINE constexpr auto get_value_in_container(auto&& container) const -> decltype(auto) requires (std::derived_from<std::decay_t<decltype(container)>, Container>) {
		return (FORWARD_AUTO(container).*member);
	}

	[[nodiscard]] FORCEINLINE auto get_offset() const -> usize {
		return reinterpret_cast<usize>(&(static_cast<Container*>(nullptr)->*member));
	}
};

template <usize N>
using SizedUnsignedIntegral = std::conditional_t<
	(N <= 8), u8, std::conditional_t<
	(N <= 16), u16, std::conditional_t<
	(N <= 32), u32, u64>>>;

// Wrapper around a type.
template <typename T>
struct Wrapper : public T {
	using T::T;

	using Type = T;

	[[nodiscard]] constexpr auto operator->() -> T* { return static_cast<T*>(this); }
	[[nodiscard]] constexpr auto operator->() const -> const T* { return static_cast<const T*>(this); }
	[[nodiscard]] constexpr auto operator*() -> T& { return *static_cast<T*>(this); }
	[[nodiscard]] constexpr auto operator*() const -> const T& { return *static_cast<const T*>(this); }
	[[nodiscard]] constexpr auto get() -> T& { return *static_cast<T*>(this); }
	[[nodiscard]] constexpr auto get() const -> const T& { return *static_cast<const T*>(this); }
};
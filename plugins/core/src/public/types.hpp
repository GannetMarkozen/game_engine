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

	FORCEINLINE constexpr auto operator()(Params&&... params) -> Return {
		return fn(data, std::forward<Params>(params)...);
	}

private:
	void* data;
	Return(*fn)(void*, Params...);
};

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

namespace cpts {
template <typename T1, typename T2>
concept ExactSame = std::same_as<T1, T2>;

template <typename T1, typename T2>
concept Same = ExactSame<std::decay_t<T1>, std::decay_t<T2>>;
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
struct Tuple;

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

template <typename T, typename... Ts>
struct Tuple<T, Ts...> {
	template <typename...>
	friend struct Tuple;

	static constexpr usize COUNT = sizeof...(Ts) + 1;

	[[nodiscard]] FORCEINLINE constexpr Tuple() = default;
	[[nodiscard]] FORCEINLINE constexpr explicit Tuple(NoInit)
		: value{NO_INIT}, next{NO_INIT} {}

	template <typename Arg, typename... Args> requires (std::constructible_from<Arg, T> && sizeof...(Ts) == sizeof...(Args))
	[[nodiscard]] FORCEINLINE constexpr explicit Tuple(Arg&& arg, Args&&... args)
		: value{std::forward<Arg>(arg)}, next{std::forward<Args>(args)...} {}

	template <template <typename...> typename Container, typename Other, typename... Others>
	requires (cpts::IsTuple<Container<Other, Others...>> && std::assignable_from<Other, T> && sizeof...(Ts) == sizeof...(Others))
	FORCEINLINE constexpr auto operator=(Container<Other, Others...>&& tuple) -> Tuple& {
		value = std::forward<Other>(tuple.value);
		next = std::forward<Container<Other, Others...>>(tuple);
		return *this;
	}

	template <typename Other, typename... Others> requires (sizeof...(Ts) == sizeof...(Others))
	FORCEINLINE constexpr auto operator==(const Tuple<Other, Others...>& other) const -> bool {
		return value == other.value && next == other.next;
	}

	template <usize I, typename Self> requires (I < sizeof...(Ts) + 1)
	[[nodiscard]] FORCEINLINE constexpr auto get(this Self&& self) -> decltype(auto) {
		if constexpr (I == 0) {
			return (std::forward<Self>(self).value);
		} else {
			return std::forward<Self>(self).next.template get<I - 1>();
		}
	}

	template <usize I, typename Self> requires (std::same_as<std::decay_t<Self>, Tuple>)
	[[nodiscard]] FORCEINLINE friend constexpr auto get(Self&& self) -> decltype(auto) {
		return std::forward<Self>(self).template get<I>();
	}

	template <typename Self>
	FORCEINLINE constexpr auto visit(this Self&& self, auto&& fn) -> void {
		std::invoke(fn, std::forward_like<Self>(self.value));
		if constexpr (sizeof...(Ts) > 0) {
			std::forward_like<Self>(self.next).visit(FORWARD_AUTO(fn));
		}
	}

	consteval auto count() const -> usize {
		return COUNT;
	}

	template <usize I>
	using TypeAtIndex = utils::TypeAtIndex<I, T, Ts...>;

private:
	T value;
	[[msvc::no_unique_address]] Tuple<Ts...> next;
};

template <>
struct Tuple<> {
	static constexpr usize COUNT = 0;
	consteval auto count() const -> usize {
		return 0;
	}

	FORCEINLINE constexpr auto visit(this auto&& self, auto&& fn) -> void {}
};

static_assert(sizeof(u32) == sizeof(Tuple<u32>), "[[no_unique_address]] attribute failed!");

//~ Support for structured bindings.
template <typename... Ts>
struct std::tuple_size<Tuple<Ts...>> : std::integral_constant<usize, sizeof...(Ts)> {};

template <usize I, typename... Ts>
struct std::tuple_element<I, Tuple<Ts...>> {
	using type = Tuple<Ts...>::template TypeAtIndex<I>;
};
//~

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

// Subclass this to make an integer type that can't be coerced into other IntAlias types.
template<std::integral T>
struct IntAlias {
	constexpr IntAlias() : value{0} {}
	explicit IntAlias(NoInit) {}
	constexpr explicit IntAlias(const std::integral auto in_value) : value{in_value} {}
	constexpr IntAlias(const IntAlias&) = default;
	constexpr IntAlias(IntAlias&&) noexcept = default;
	constexpr auto operator=(const IntAlias&) -> IntAlias& = default;
	constexpr auto operator=(IntAlias&&) noexcept -> IntAlias& = default;

	[[nodiscard]] FORCEINLINE constexpr auto operator==(const std::integral auto other) const { return value == other; }

	template <std::integral Other>
	[[nodiscard]] FORCEINLINE constexpr auto operator==(const IntAlias<Other>& other) const { return value == other.value; }

	[[nodiscard]] FORCEINLINE constexpr auto operator!=(const std::integral auto other) const { return value != other; }

	template<std::integral Other>
	[[nodiscard]] FORCEINLINE constexpr auto operator!=(const IntAlias<Other>& other) const { return value != other.value; }

	[[nodiscard]] FORCEINLINE constexpr auto operator<=>(const std::integral auto other) const { return value <=> other; }

	template<std::integral Other>
	[[nodiscard]] FORCEINLINE constexpr auto operator<=>(const IntAlias<Other>& other) const { return value <=> other.value; }

	template<typename Other>
	requires std::is_arithmetic_v<T>
	[[nodiscard]]
	FORCEINLINE constexpr operator Other() const { return static_cast<Other>(value); }

	[[nodiscard]] FORCEINLINE constexpr operator T&() { return value; }
	[[nodiscard]] FORCEINLINE constexpr operator const T&() const { return value; }

	FORCEINLINE constexpr auto set_value(const T new_value) -> void { value = new_value; }
	[[nodiscard]] FORCEINLINE constexpr auto get_value() const -> T { return value; }

private:
	T value;
};

namespace std {
template<std::integral T>
struct hash<IntAlias<T>> {
	[[nodiscard]] FORCEINLINE auto operator()(const IntAlias<T> value) { return std::hash<T>{}(value.get_value()); }
};
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
		if constexpr (!cpts::ExactSame<T, OtherT> || !cpts::ExactSame<M, OtherM>) {
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
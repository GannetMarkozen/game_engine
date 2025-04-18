#pragma once

#include "types.hpp"

namespace utils {
template <usize N, usize START = 0, usize INCREMENT = 1> requires (INCREMENT > 0)
FORCEINLINE constexpr auto unrolled_for(auto&& fn, auto&&... args) -> void {
	if constexpr (N < START) {
		using Return = decltype(fn.template operator()<START>(FORWARD_AUTO(args)...));

		if constexpr (std::convertible_to<Return, bool>) {
			if (static_cast<bool>(fn.template operator()<START>(FORWARD_AUTO(args)...))) {
				return;
			}
		} else {
			fn.template operator()<START>(FORWARD_AUTO(args)...);
		}

		unrolled_for<N, START + INCREMENT>(FORWARD_AUTO(fn), FORWARD_AUTO(args)...);
	}
}

template <typename T>
[[nodiscard]] consteval auto get_raw_type_name() -> StringView {
	const StringView name = __PRETTY_FUNCTION__;
#if defined(__clang__) || defined(__GNUC__)
	// Extract between "T = " and "]"
	const auto start = name.find('=') + 2;
	const auto end = name.rfind(']');
	return name.substr(start, end - start);
#elif defined(_MSC_VER)
	// Extract between "type_name<" and ">(void)"
	const auto start = name.find('<') + 1;
	const auto end = name.rfind('>');
	return name.substr(start, end - start);
#else
	return name;
#endif
}

[[nodiscard]] constexpr auto to_compact_file_name(const StringView file_name) -> StringView {
	const usize offset = file_name.rfind('/') + 1;
	return file_name.substr(offset, offset - file_name.size());
}

namespace impl {
constexpr StringView TYPE_NAME_REDIRECTS[][2] = {
	{"std::vector", "Array"},
	{"std::array", "StaticArray"},
	{"std::span", "Span"},
	{"std::unordered_map", "Map"},
	{"std::unordered_set", "Set"},
	{"std::optional", "Optional"},
	{"std::pair", "Pair"},
	{"std::basic_string<char>", "String"},
	{"std::basic_string_view<char>", "StringView"},
	{"std::variant", "Variant"},
	{"std::unique_ptr", "UniquePtr"},
	{"std::shared_ptr", "SharedPtr"},
	{"std::weak_ptr", "WeakPtr"},
	{"unsigned long long", "u64"},
	{"unsigned long", "u32"},
	{"unsigned int", "u32"},
	{"unsigned short", "u16"},
	{"unsigned char", "u8"},
	{"unsigned", "u32"},
	{"long long", "i64"},
	{"long", "i32"},
	{"int", "i32"},
	{"short", "i16"},
	{"char", "i8"},
	{"float", "f32"},
	{"double", "f64"},
};

constexpr StringView SYMBOL_REDIRECTS[][2] = {
	{" &", "&"},
	{" *", "*"},
};

constexpr StringView TYPE_DELIMITER_CHARS{"<>: *&,"};
}

template <typename T>
[[nodiscard]] FORCEINLINE auto get_type_name() -> StringView {
	static const auto NAME = [] {
		auto name = String{get_raw_type_name<T>()};
		for (const auto& [redirect_from, redirect_to] : impl::TYPE_NAME_REDIRECTS) {
			usize current_position = 0;
			while ((current_position = name.find(redirect_from, current_position)) != String::npos) {
				if ((current_position == 0 || impl::TYPE_DELIMITER_CHARS.contains(name[current_position - 1]) &&
					(current_position == name.size() || impl::TYPE_DELIMITER_CHARS.contains(name[current_position + redirect_from.size()])))) {
					name.replace(current_position, redirect_from.size(), redirect_to);
				} else {
					current_position += redirect_from.size();
				}
			}
		}

		for (const auto& [redirect_from, redirect_to] : impl::SYMBOL_REDIRECTS) {
			usize current_position = 0;
			while ((current_position = name.find(redirect_from, current_position)) != String::npos) {
				name.replace(current_position, redirect_from.size(), redirect_to);
			}
		}

		return name;
	}();

	return StringView{NAME};
}

template <typename>
[[nodiscard]] consteval auto is_container_type() -> bool { return false; }

template <template <typename...> typename>
[[nodiscard]] consteval auto is_container_type() -> bool { return true; }

template <typename T>
[[nodiscard]] consteval auto get_raw_container_name() -> StringView {
	const StringView name = get_raw_type_name<T>();
	const auto end_scope = name.rfind(':');
	const auto end = name.rfind('<', end_scope);
	return name.substr(0, end);
}


template <typename T, typename Container> requires (is_container_type<Container>())
[[nodiscard]] consteval auto contains_type_in_container() -> usize {
	return []<template <typename...> typename DeTemplatedContainer, typename... Ts>(DeTemplatedContainer<Ts...>) consteval -> usize {
		return utils::contains_type<T, Ts...>();
	}(std::declval<Container>());
}

template <typename T, typename Container> requires (contains_type_in_container<T, Container>())
[[nodiscard]] consteval auto index_of_type_in_container() -> usize {
	return []<template <typename...> typename DeTemplatedContainer, typename... Ts>(DeTemplatedContainer<Ts...>) consteval -> usize {
		return utils::index_of_type<T, Ts...>();
	}(std::declval<Container>());
}

template <auto FN, typename... Ts> requires (contains_type_by_predicate<Ts...>(FN))
using FindTypeByPredicate = TypeAtIndex<index_of_type_by_predicate<Ts...>(FN), Ts...>;

template <typename T> requires (is_container_type<T>())
FORCEINLINE constexpr auto for_each_type_in_container(auto&& fn, auto&&... args) -> void {
	[&]<template <typename...> typename Container, typename... Ts>(Container<Ts...>) constexpr {
		(fn.template operator()<Ts>(FORWARD_AUTO(args)...), ...);
	}(std::declval<T>());
}

namespace impl { template <typename>
struct ParamPackGetter { static_assert(false, "Not container type!"); };

template <template <typename...> typename Container, typename... Ts>
struct ParamPackGetter<Container<Ts...>> {
	FORCEINLINE static constexpr auto get(auto&& fn, auto&&... args) -> decltype(auto) {
		return (fn.template operator()<Ts...>(FORWARD_AUTO(args)...));
	}
};

template <typename>
struct ParamPackValueGetter { static_assert(false, "Not container type!"); };

template <template <auto...> typename Container, auto... Vs>
struct ParamPackValueGetter<Container<Vs...>> {
	FORCEINLINE static constexpr auto get(auto&& fn, auto&&... args) -> decltype(auto) {
		return (fn.template operator()<Vs...>(FORWARD_AUTO(args)...));
	}
};
}

template <typename T>
FORCEINLINE constexpr auto make_param_pack(auto&& fn, auto&&... args) -> decltype(auto) {
	return (impl::ParamPackGetter<T>::template get(FORWARD_AUTO(fn), FORWARD_AUTO(args)...));
}

template <typename T>
FORCEINLINE constexpr auto get_value_param_pack(auto&& fn, auto&&... args) -> decltype(auto) {
	return (impl::ParamPackValueGetter<T>::template get(FORWARD_AUTO(fn), FORWARD_AUTO(args)...));
}

template <usize COUNT, std::integral T = usize>
FORCEINLINE constexpr auto make_index_sequence_param_pack(auto&& fn, auto&&... args) -> decltype(auto) {
	return [&]<T... Is>(std::integer_sequence<T, Is...>) constexpr -> decltype(auto) {
		return (fn.template operator()<Is...>(FORWARD_AUTO(args)...));
	}(std::make_integer_sequence<T, COUNT>{});
}

template <typename... Ts>
FORCEINLINE constexpr auto visit(const Variant<Ts...>& variant, auto&& fn) -> decltype(auto) {
	return (std::visit(variant, FORWARD_AUTO(fn)));
}

template <typename... Ts>
FORCEINLINE constexpr auto visit(Variant<Ts...>& variant, auto&& fn) -> decltype(auto) {
	return (std::visit(variant, FORWARD_AUTO(fn)));
}

template <typename... Ts>
FORCEINLINE constexpr auto visit(const Tuple<Ts...>& tuple, auto&& fn) -> void {
	make_index_sequence_param_pack<sizeof...(Ts)>([&]<usize... Is>() constexpr {
		(fn(std::get<Is>(tuple)), ...);
	});
}

template <typename... Ts>
FORCEINLINE constexpr auto visit(Tuple<Ts...>& tuple, auto&& fn) -> void {
	make_index_sequence_param_pack<sizeof...(Ts)>([&]<usize... Is>() constexpr {
		(fn(std::get<Is>(tuple)), ...);
	});
}

namespace impl {
template <typename>
struct CountImpl;

template <template <typename...> typename Container, typename... Ts>
struct CountImpl<Container<Ts...>> {
	static constexpr usize VALUE = sizeof...(Ts);
};
}

template <typename T>
consteval auto count() -> usize {
	return impl::CountImpl<std::decay_t<T>>::VALUE;
}

namespace impl {
template <typename, typename>
struct Concat;

template <template <typename...> typename ContainerA, template <typename...> typename ContainerB, typename... As, typename... Bs>
struct Concat<ContainerA<As...>, ContainerB<Bs...>> {
	static_assert(std::is_same_v<ContainerA<As...>, ContainerB<As...>>, "Concat called with different containers!");

	using Type = ContainerA<As..., Bs...>;
};
}

// Concatenates types within a container.
template <typename A, typename B>
using Concat = typename impl::Concat<A, B>::Type;

namespace impl {
template <auto, usize, usize, typename>
struct ConditionalSwap;

template <auto FN, usize A, usize B, template <typename...> typename Container, typename... Ts>
struct ConditionalSwap<FN, A, B, Container<Ts...>> {
	using Type = Container<Ts...>;
};

template <auto FN, usize A, usize B, template <typename...> typename Container, typename... Ts> requires (FN.template operator()<utils::TypeAtIndex<A, Ts...>, utils::TypeAtIndex<A, Ts...>>())
struct ConditionalSwap<FN, A, B, Container<Ts...>> {
	using Type =
		Container<std::conditional_t<(utils::index_of_type<Ts>() == A), utils::TypeAtIndex<A, Ts...>,
		std::conditional_t<(utils::index_of_type<Ts>() == B), utils::TypeAtIndex<B, Ts...>, Ts>>...>;
};
}

// Retrieves the function signature of any function, member-function, or callable.
template <typename>
struct FnSig;

template <typename Return, typename... Args>
struct FnSig<Return(*)(Args...)> {
	using ReturnType = Return;
	using ArgTypes = Tuple<Args...>;
	using ClassType = void;
	static constexpr bool CONST = false;
};

template <typename Return, typename Class, typename... Args>
struct FnSig<Return(Class::*)(Args...)> {
	using ReturnType = Return;
	using ArgTypes = Tuple<Args...>;
	using ClassType = Class;
	static constexpr bool CONST = false;
};

template <typename Return, typename Class, typename... Args>
struct FnSig<Return(Class::*)(Args...) const> {
	using ReturnType = Return;
	using ArgTypes = Tuple<Args...>;
	using ClassType = Class;
	static constexpr bool CONST = true;
};

template <typename T> requires requires { &T::operator(); }
struct FnSig<T> : public FnSig<decltype(&T::operator())> {};

namespace impl {
template <auto, typename...>
struct Filter;

template <auto PREDICATE, typename T, typename... Ts>
struct Filter<PREDICATE, T, Ts...> {
	using Types = std::conditional_t<PREDICATE.template operator()<T>(), ::utils::Concat<Tuple<T>, typename Filter<PREDICATE, Ts...>::Types>, typename Filter<PREDICATE, Ts...>::Types>;
};

template <auto PREDICATE, typename T>
struct Filter<PREDICATE, T> {
	using Types = std::conditional_t<PREDICATE.template operator()<T>(), Tuple<T>, Tuple<>>;
};

template <auto PREDICATE>
struct Filter<PREDICATE> {
	using Types = Tuple<>;
};

template <auto, typename>
struct FilterContainer;

template <auto PREDICATE, template <typename...> typename Container, typename... Ts>
struct FilterContainer<PREDICATE, Container<Ts...>> {
	using Types = typename Filter<PREDICATE, Ts...>::Types;
};
}

template <auto PREDICATE, typename... Ts>
using Filter = typename impl::Filter<PREDICATE, Ts...>::Types;

template <auto PREDICATE, typename T>
using FilterContainer = typename impl::FilterContainer<PREDICATE, T>::Types;
}
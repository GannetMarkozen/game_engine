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

namespace impl {

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
}
#pragma once

#include <variant>
#include "assert.hpp"

template<typename... Ts>
class Variant : public std::variant<Ts...> {
public:
	using std::variant<Ts...>::variant;

	template<typename T>
	static consteval fn contains() -> bool {
		return (std::is_same_v<T, Ts> || ...);
	}

	template<typename T>
	static consteval fn index_of() -> usize {
		static_assert(contains<T>(), "Variant does not contain this type!");
		usize index = 0;
		const bool found = ((std::is_same_v<T, Ts> ? true : [&] { ++index; return false; }()) || ...);
		return index;
	}

	template<usize INDEX>
	using TypeAt = std::tuple_element_t<INDEX, std::tuple<Ts...>>;

	template<typename T>
	[[nodiscard]]
	FORCEINLINE constexpr fn get() -> T& {
		ASSERTF(index() == index_of<T>(), "Attempted to get index {} when the variant contains index {}!", index_of<T>(), index());
		return std::get<T>(*this);
	}

	template<typename T>
	[[nodiscard]]
	FORCEINLINE constexpr fn get() const -> const T& {
		ASSERTF(index() == index_of<T>(), "Attempted to get index {} when the variant contains index {}!", index_of<T>(), index());
		return std::get<T>(*this);
	}

	template<Invokable Func, typename... Args>
	FORCEINLINE constexpr fn match(Func&& func, Args&&... args) const -> decltype(std::forward<Func>(func).template operator()<TypeAt<0>>(std::forward<Args>(args)...)) {
		using Return = decltype(std::forward<Func>(func).template operator()<TypeAt<0>>(std::forward<Args>(args)...));

		static constexpr Return(*table[])(Func&&, Args&&...) {
			[](Func&& func, Args&&... args) -> Return {
				if constexpr (std::is_same_v<Return, void>) {
					std::forward<Func>(func).template operator()<Ts>(std::forward<Args>(args)...);
				} else {
					return std::forward<Func>(func).template operator()<Ts>(std::forward<Args>(args)...);
				}
			}...
		};
		static_assert(std::size(table) == sizeof...(Ts));

		if constexpr (std::is_same_v<Return, void>) {
			table[index()](std::forward<Func>(func), std::forward<Args>(args)...);
		} else {
			return table[index()](std::forward<Func>(func), std::forward<Args>(args)...);
		}
	}
};
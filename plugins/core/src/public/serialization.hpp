#pragma once

#include <limits>
#include <ostream>
#include <ranges>
#include "types.hpp"
#include "static_reflection.hpp"
#include "utils.hpp"

template <typename T> requires (std::integral<T> || std::floating_point<T>)
FORCEINLINE constexpr auto serialize_json(std::ostream& stream, const T value) -> std::ostream& {
	return stream << value;
}

FORCEINLINE constexpr auto serialize_json(std::ostream& stream, const bool value) -> std::ostream& {
	return stream << (value ? "true" : "false");
}

FORCEINLINE constexpr auto serialize_json(std::ostream& stream, const String& value) -> std::ostream& {
	stream << '"';
	stream << value;
	stream << '"';

	return stream;
}

// Json serialize overload for containers.
template <std::ranges::range Container>
FORCEINLINE constexpr auto serialize_json(std::ostream& stream, const Container& container) -> std::ostream& {
	stream << '[';

	for (auto it = container.begin(); it != container.end(); ++it) {
		serialize_json(stream, *it);

		// Don't add comma for last element.
		if (it != std::prev(container.end())) {
			stream << ',';
		}
	}

	stream << ']';

	return stream;
}

template <typename... Ts>
FORCEINLINE constexpr auto serialize_json(std::ostream& stream, const Tuple<Ts...>& value) -> std::ostream& {
	stream << '{';

	utils::make_index_sequence_param_pack<sizeof...(Ts)>([&]<usize... Is>() constexpr {
		([&] {
			stream << '<';
			stream << Is;
			stream << '>';
			stream << ':';

			serialize_json(stream, value.template get<Is>());

			if constexpr (Is != sizeof...(Ts) - 1) {
				stream << ',';
			}
		}(), ...);
	});

	stream << '}';

	return stream;
}

// Json serialize overload for objects.
template <cpts::Reflected T> requires (std::is_class_v<T>)
FORCEINLINE constexpr auto serialize_json(std::ostream& stream, const T& value) -> std::ostream& {
	stream << '{';

	meta::make_reflected_members_param_pack<std::decay_t<T>>([&]<typename... Members>() constexpr {
		static constexpr usize LAST_INDEX_TO_SERIALIZE = []<usize I>(this auto&& self) consteval {
			if constexpr (utils::TypeAtIndex<I, Members...>::template has_attribute_by_name<attributes::NOT_SERIALIZED>()) {
				if constexpr (I != 0) {
					return self.template operator()<I - 1>();
				} else {
					return 0;
				}
			} else {
				return I;
			}
		}.template operator()<sizeof...(Members) - 1>();

		([&] {
			if constexpr (!Members::template has_attribute_by_name<attributes::NOT_SERIALIZED>()) {
				stream << '"';
				stream << Members::NAME.view();
				stream << '"';
				stream << ':';

				serialize_json(stream, Members::get_value_in_container(value));

				static constexpr auto INDEX = utils::index_of_type<Members, Members...>();
				if constexpr (INDEX != LAST_INDEX_TO_SERIALIZE) {
					static_assert(INDEX < LAST_INDEX_TO_SERIALIZE);

					stream << ',';
				}
			}
		}(), ...);
	});

	stream << '}';

	return stream;
}
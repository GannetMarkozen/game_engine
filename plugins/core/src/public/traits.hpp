#pragma once

#include "types.hpp"

namespace cpts {
template <typename T>
concept HasToString = requires(const T t, String out_str) {
	to_string(t, out_str);
};

template <typename T>
concept HasFromString = requires(T t, const StringView in_str) {
	from_string(t, in_str);
};
}
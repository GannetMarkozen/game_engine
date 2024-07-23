#pragma once

#include <ranges>
#include "types.hpp"

namespace cpts {
template <typename R, typename T>
concept Range = std::ranges::range<R> && std::same_as<std::decay_t<std::ranges::range_value_t<R>>, T>;
}
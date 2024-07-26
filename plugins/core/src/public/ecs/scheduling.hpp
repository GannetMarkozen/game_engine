#pragma once

#include "type_registry.hpp"
#include "threading/thread_safe_types.hpp"

namespace ecs {
namespace cpts {
template <typename T>
concept ExecutionGroup = ::cpts::EnumWithCount<T>;
}
}
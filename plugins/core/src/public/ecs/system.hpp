#pragma once

#include "types.hpp"

namespace ecs {
struct SystemId final : public IntAlias<u16> {
	using IntAlias<u16>::IntAlias;
};

struct SystemExecutionInfo {
	Fn<void()> fn;
};
}
#pragma once

#include "world.hpp"

namespace ecs {
struct SystemContext {
	

	World& [[clang::lifetimebound]] world;
	SystemId this_system = SystemId::invalid_id();
};
}
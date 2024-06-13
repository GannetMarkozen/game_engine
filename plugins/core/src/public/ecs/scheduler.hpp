#pragma once

#include "../defines.hpp"
#include "system.hpp"

namespace ecs {
struct Scheduler {
	fn schedule_system(SystemInfo system) -> void;

private:
	
};
}
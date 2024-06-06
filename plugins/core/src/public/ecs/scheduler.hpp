#pragma once

#include "../defines.hpp"
#include "system.hpp"

namespace core::ecs {
struct Scheduler {
	fn schedule_system(SystemInfo system) -> void;

private:
	
};
}
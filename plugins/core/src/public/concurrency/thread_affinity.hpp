#pragma once

#include <thread>
#include "defines.hpp"

namespace utils {
fn set_thread_affinity(std::thread& thread, const usize core_id) -> void;
}
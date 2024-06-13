#pragma once

#include "types.hpp"
namespace impl {
EXPORT_API inline constinit std::atomic<u32> thread_incrementer = 0;
thread_local inline const u32 thread_index = thread_incrementer++;
}

// Arbitrary index assigned to each thread.
// @TODO: Make this not keep incrementing forever as threads are created / destroyed (shouldn't matter for now or probably ever).
[[nodiscard]]
FORCEINLINE fn get_thread_index() -> u32 {
	return impl::thread_index;
}

[[nodiscard]]
FORCEINLINE fn get_num_threads() -> u32 {
	return impl::thread_incrementer.load(std::memory_order_relaxed);
}
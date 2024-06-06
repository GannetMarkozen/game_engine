#if defined(_WIN32) || defined(_WIN64)
#include <Windows.h>
#elif defined(__linux__)
#include <pthread.h>
#endif

#include "assert.hpp"

#include "concurrency/thread_affinity.hpp"

namespace core::utils {

#if defined(_WIN32) || defined(_WIN64)

fn set_thread_affinity(std::thread& thread, const usize core_id) -> void {
	ASSERTF(core_id <= GetCurrentProcessorNumber(), "Attempted to assign core id {} thread affinity when the core count is {}!", core_id, GetCurrentProcessorNumber());

	const DWORD_PTR result = SetThreadAffinityMask(thread.native_handle(), 1 << core_id);
	ASSERT(result != 0);
}

#elif defined(__linux__)

fn set_thread_affinity(std::thread& thread, const usize core_id) -> void {
	cpu_set_t cpu_set;
	CPU_ZERO(&cpu_set);
	CPU_SET(core_id, &cpu_set);

	const auto result = pthread_setaffinity_np(thread.native_handle(), sizeof(cpu_set_t), &cpu_set);
	ASSERT(result != 0);
}

#else

static_assert(false, "Unknown platform");

#endif

}
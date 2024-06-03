#pragma once

#include "defines.hpp"

#define ASSERTIONS_ENABLED DEBUG_BUILD

#if ASSERTIONS_ENABLED

#define ASSERT(condition) { \
		if (!(condition)) [[unlikely]] { \
			fmt::print(fmt::fg(fmt::color::red), "ERROR: ASSERTION FAILED!: ({})\nFILE: {}\nLINE: {}\nCALLSTACK:\n{}\n", \
				#condition, __FILE_NAME__, __LINE__, std::to_string(std::stacktrace::current())); \
			abort(); \
			UNREACHABLE; \
		} \
	}

#define ASSERTF(condition, ...) { \
		if (!(condition)) [[unlikely]] { \
			fmt::print(fmt::fg(fmt::color::red), "ERROR: ASSERTION FAILED!: ({}). MSG: \"{}\"\nFILE: {}\nLINE: {}\nCALLSTACK:\n{}\n", \
				#condition, fmt::format(__VA_ARGS__), __FILE_NAME__, __LINE__, std::to_string(std::stacktrace::current())); \
			abort(); \
			UNREACHABLE; \
		} \
	}

#define ASSERT_UNREACHABLE { \
		fmt::print(fmt::fg(fmt::color::red), "ERROR: HIT \"UNREACHABLE\" CODE BLOCK!\nFILE: {}\nLINE: {}\nCALLSTACK:\n{}\n", \
			__FILE_NAME__, __LINE__, std::to_string(std::stacktrace::current())); \
		abort(); \
		UNREACHABLE; \
	}

#define ASSERT_UNREACHABLE_DEFAULT default: ASSERT_UNREACHABLE

#else

#define ASSERT(...)
#define ASSERTF(...)
#define ASSERT_UNREACHABLE UNREACHABLE;
#define ASSERT_UNREACHABLE_DEFAULT default: { UNREACHABLE; }

#endif
#pragma once

#include "defines.hpp"

#define ASSERTIONS_ENABLED DEBUG_BUILD

#if ASSERTIONS_ENABLED

#define FATAL_ERRORF(format, ...) { \
		utils::noinline_exec([](auto&&... args) { \
			fmt::print(fmt::fg(fmt::color::red), format, FORWARD_AUTO(args)...); \
			abort(); \
		}, ##__VA_ARGS__); \
		UNREACHABLE; \
	}

#define ASSERT(condition) { \
		if (!(condition)) [[unlikely]] { \
			FATAL_ERRORF("ERROR: ASSERTION FAILED!: ({})\nFILE: {}\nLINE: {}\nCALLSTACK:\n{}\n", \
				#condition, __FILE_NAME__, __LINE__, std::to_string(std::stacktrace::current())); \
		} \
	}

#define ASSERTF(condition, ...) { \
		if (!(condition)) [[unlikely]] { \
			FATAL_ERRORF("ERROR: ASSERTION FAILED!: ({}). MSG: \"{}\"\nFILE: {}\nLINE: {}\nCALLSTACK:\n{}\n", \
				#condition, fmt::format(__VA_ARGS__), __FILE_NAME__, __LINE__, std::to_string(std::stacktrace::current())); \
		} \
	}

#define ASSERT_UNREACHABLE { \
		FATAL_ERRORF("ERROR: HIT \"UNREACHABLE\" CODE BLOCK!\nFILE: {}\nLINE: {}\nCALLSTACK:\n{}\n", \
			__FILE_NAME__, __LINE__, std::to_string(std::stacktrace::current())); \
	}

#define ASSERT_UNREACHABLE_DEFAULT default: ASSERT_UNREACHABLE

#define UNIMPLEMENTED { \
		FATAL_ERRORF("ERROR: HIT \"UNIMPLEMENTED\" CODE BLOCK!\nFILE: {}\nLINE: {}\nCALLSTACK:\n{}\n", \
			__FILE_NAME__, __LINE__, std::to_string(std::stacktrace::current())); \
	}

#define WARN(format, ...) { \
		utils::noinline_exec([](auto&&... args) { \
			fmt::print(fmt::fg(fmt::color::yellow), format, FORWARD_AUTO(args)...); \
		}, ##__VA_ARGS__); \
	}

#else

#define FATAL_ERRORF(...)

#define ASSERT(...)
#define ASSERTF(...)
#define ASSERT_UNREACHABLE UNREACHABLE;
#define ASSERT_UNREACHABLE_DEFAULT default: { UNREACHABLE; }
#define UNIMPLEMENTED UNREACHABLE;

#define WARN(...)

#endif
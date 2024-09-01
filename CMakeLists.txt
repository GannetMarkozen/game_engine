cmake_minimum_required(VERSION 3.6.0)
project(game_engine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_VERBOSE_MAKEFILE TRUE)

# Set the built executable directory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)

# Set C++ standard to 23.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O1")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -g -O2")

# Variable to enable/disable address sanitizer (only in debug configuration).
set(ASAN_ENABLED TRUE)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Enable SSE4, disable exceptions, disable RTTI.
    add_compile_options(-msse4.1 -msse4.2 -fno-exceptions -fno-rtti)

    if (ASAN_ENABLED)
        # Having either -D_DLL or -D_DEBUG breaks asan in weird ways and is passed in by default (this took me forever to figure out).
        string(REPLACE "-D_DLL" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        string(REPLACE "-D_DEBUG" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")

        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -fno-omit-frame-pointer")
        set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address -fno-omit-frame-pointer")
    endif()

    # Enable various optimization flags for release.
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -flto -ftree-vectorize -funroll-loops")
elseif (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    add_compile_options(/arch:SSE4.2 /EHs-c- /GR-)
else()
    message(FATAL_ERROR "Unknown compiler ${CMAKE_CXX_COMPILER_ID}!")
endif()

add_executable(game_engine src/private/main.cpp)

# Configure library definitions.
add_compile_definitions(
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
    GLFW_INCLUDE_VULKAN
    FMT_EXCEPTIONS=0 # Disable exceptions in fmt.
	FMT_HEADER_ONLY
	TF_DISABLE_EXCEPTION_HANDLING # Disable exceptions for TaskFlow.
    FOONATHAN_HAS_EXCEPTION_SUPPORT=0 # Disable excepetions for memory allocators.
)

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
	add_compile_definitions(
		DEBUG_BUILD=1
		RELEASE_BUILD=0
	)
elseif (CMAKE_BUILD_TYPE STREQUAL "Release")
	add_compile_definitions(
		DEBUG_BUILD=0
		RELEASE_BUILD=1
	)
else()
	message(FATAL_ERROR "Unknown build configuration")
endif()

include(FetchContent)

FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG bf71a834948186f4097caa076cd2663c69a10e1e #refs/tags/1.0.1
)

FetchContent_Declare(
    flecs
    GIT_REPOSITORY https://github.com/SanderMertens/flecs.git
    GIT_TAG master
)
FetchContent_MakeAvailable(glm)
FetchContent_MakeAvailable(flecs)

# TMP: Disable warnings from strncpy in vulkan.hpp.
add_definitions(-D_CRT_SECURE_NO_WARNINGS)

# Plugins.
add_subdirectory(plugins/core)
add_subdirectory(plugins/default_plugins)
add_subdirectory(plugins/vulkan_renderer)
add_subdirectory(plugins/gameplay_framework)

target_link_libraries(game_engine
    # Third-party.
	#PRIVATE glm::glm
    PRIVATE flecs_static

    PRIVATE default_plugins
)
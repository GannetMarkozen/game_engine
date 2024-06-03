cmake_minimum_required(VERSION 3.6.0)
project(game_engine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_VERBOSE_MAKEFILE TRUE)

# Set the built executable directory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# Set C++ standard to 23.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Set compile flags for different build configurations.
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -g -O2")

# Enable SSE4, disable exceptions, disable RTTI.
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-msse4.1 -msse4.2 -fno-exceptions -fno-rtti)

    # Enable various optimization flags.
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -flto -ftree-vectorize -funroll-loops")
elseif (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    add_compile_options(/arch:SSE4.2 /EHs-c- /GR-)
else()
    message(FATAL_ERROR "Unknown compiler ${CMAKE_CXX_COMPILER_ID}!")
endif()

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
FetchContent_Declare(
    VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG v1.3.266
)
FetchContent_MakeAvailable(glm)
FetchContent_MakeAvailable(flecs)
FetchContent_MakeAvailable(VulkanHeaders)

if (NOT DEFINED ENV{VULKAN_SDK})
    message(FATAL_ERROR "VULKAN_SDK environment variable not set! Download Vulkan SDK v1.3.266.")
endif()

set(VULKAN_SDK "$ENV{VULKAN_SDK}")

link_directories("${VULKAN_SDK}/Lib")
find_package(Vulkan REQUIRED)

add_executable(game_engine src/private/main.cpp)

# Include public and private folders.
# Include Vulkan headers library.
target_include_directories(game_engine
    PUBLIC src/public
    PRIVATE src/private
    PRIVATE ${VulkanHeaders_SOURCE_DIR}/include

    # Add plugins directory.
    PUBLIC plugins
)

# Plugins.
add_subdirectory(plugins/core)

add_library(taskflow INTERFACE)
target_include_directories(taskflow INTERFACE third_party/taskflow-master/taskflow)

target_link_libraries(game_engine
    # Third-party.
	PRIVATE glm::glm
    PRIVATE flecs_static
	PRIVATE Vulkan::Vulkan

    # Plugins.
    PRIVATE core
)
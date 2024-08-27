cmake_minimum_required(VERSION 3.9.0)

# Set C++ standard to 23.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party)

project(vulkan_renderer VERSION 1.0)

include(FetchContent)

FetchContent_Declare(
	VulkanHeaders
	GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
	GIT_TAG v1.3.266
)
FetchContent_Declare(
	VulkanMemoryAllocator
	GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
	GIT_TAG v3.1.0
)
FetchContent_Declare(
  SDL2
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-2.30.6
)
FetchContent_Declare(
	stb
	GIT_REPOSITORY https://github.com/nothings/stb.git
	GIT_TAG master
)
FetchContent_Declare(
	tinyobjloader
	GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader.git
	GIT_TAG v2.0.0rc13
)
FetchContent_MakeAvailable(VulkanHeaders)
FetchContent_MakeAvailable(VulkanMemoryAllocator)
FetchContent_MakeAvailable(SDL2)
FetchContent_MakeAvailable(stb)
FetchContent_MakeAvailable(tinyobjloader)

# Gather all cpp files within the src/private directory.
file(GLOB_RECURSE SOURCES src/private/*.cpp)

add_library(vulkan_renderer STATIC ${SOURCES})

target_include_directories(vulkan_renderer
	PUBLIC src/public
)

option(BUILD_SHARED_LIBS "Build using shared libraries." ON)

if (NOT DEFINED ENV{VULKAN_SDK})
	message(FATAL_ERROR "VULKAN_SDK environment variable not set! Download Vulkan SDK v1.3.266.")
endif()

set(VULKAN_SDK "$ENV{VULKAN_SDK}")

link_directories("${VULKAN_SDK}/Lib")

find_package(Vulkan REQUIRED)

add_subdirectory("third_party/vk-bootstrap")

# Include Vulkan SDK directories.
target_include_directories(vulkan_renderer
	PUBLIC "${VULKAN_SDK}/Include"# C API.
	PUBLIC "${VulkanHeaders_SOURCE_DIR}/include"# C++ API.
	PUBLIC "${vma_SOURCE_DIR}/include"
	PUBLIC "${stb_SOURCE_DIR}"
	PUBLIC "${THIRD_PARTY}/vk-bootstrap"
)

target_link_libraries(vulkan_renderer
	# Plugins.
	PUBLIC core
	PUBLIC gameplay_framework

	# Other dependencies.
	PRIVATE Vulkan::Vulkan
	PRIVATE GPUOpen::VulkanMemoryAllocator
	PRIVATE vk-bootstrap::vk-bootstrap
	PRIVATE SDL2::SDL2
	PRIVATE tinyobjloader
)
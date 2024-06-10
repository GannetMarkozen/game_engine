cmake_minimum_required(VERSION 3.9.0)

# Set C++ standard to 23.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party)

project(core VERSION 1.0)

# Gather all cpp files within the src/private directory.
file(GLOB_RECURSE SOURCES src/private/*.cpp)

add_library(core STATIC ${SOURCES})

target_include_directories(core PUBLIC src/public)

option(BUILD_SHARED_LIBS "Build using shared libraries." ON)

include(FetchContent)

FetchContent_Declare(
	fmt
	GIT_REPOSITORY https://github.com/fmtlib/fmt.git
	GIT_TAG 10.1.1
)
FetchContent_Declare(
	concurrentqueue
	GIT_REPOSITORY https://github.com/cameron314/concurrentqueue
	GIT_TAG v1.0.4
)
FetchContent_MakeAvailable(fmt)
FetchContent_MakeAvailable(concurrentqueue)

target_link_libraries(core
	PUBLIC fmt::fmt
	PUBLIC concurrentqueue
)
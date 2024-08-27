cmake_minimum_required(VERSION 3.9.0)

# Set C++ standard to 23.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party)

project(gameplay_framework VERSION 1.0)

# Gather all cpp files within the src/private directory.
file(GLOB_RECURSE SOURCES src/private/*.cpp)

add_library(gameplay_framework STATIC ${SOURCES})

target_include_directories(gameplay_framework
	PUBLIC src/public
)

option(BUILD_SHARED_LIBS "Build using shared libraries." ON)

include(FetchContent)

target_link_libraries(gameplay_framework
	PRIVATE core
)
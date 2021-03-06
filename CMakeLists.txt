cmake_minimum_required(VERSION 2.6)
project(ao)
set(CMAKE_BUILD_TYPE RELEASE)

################################################################################

set(CMAKE_CXX_FLAGS "-Wall -Wextra -Wno-comment -g -fPIC -pedantic -std=c++11 -Werror=switch -march=native")
set(CMAKE_CXX_FLAGS_RELEASE  "-O3 -DRELEASE")
set(CMAKE_CXX_FLAGS_DEBUG    "-O0")

include_directories(SYSTEM vendor)

################################################################################

if (APPLE)
    set(CMAKE_MACOSX_RPATH ON)
endif()

################################################################################

# Build the ao kernel and ui libraries
add_subdirectory(kernel)
add_subdirectory(ui)

# Build the test suite
add_subdirectory(test)

# Build the ao-guile binding
add_subdirectory(bind)

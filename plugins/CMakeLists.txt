###############################################################################
# rAthena Plugin Build System
#
# Usage (standalone — separate from the main rAthena CMake project):
#   mkdir build && cd build
#   cmake -DRATHENA_SRC=<path/to/rathena/src> ..
#   cmake --build .
#
# Usage (integrated — from the rAthena root build directory):
#   cmake -DBUILD_PLUGINS=ON <rathena-root>
#
###############################################################################

cmake_minimum_required(VERSION 3.13)
project(rAthenaPlugins CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Path to rAthena src/ directory.
# Defaults to sibling of this plugins/ directory when building standalone.
if(NOT DEFINED RATHENA_SRC)
    set(RATHENA_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src"
        CACHE PATH "Path to rAthena src/ directory")
endif()

if(NOT EXISTS "${RATHENA_SRC}/map/plugin.hpp")
    message(FATAL_ERROR
        "RATHENA_SRC does not point to a valid rAthena src/ directory.\n"
        "Set -DRATHENA_SRC=<path/to/rathena/src> or run cmake from the rAthena root.")
endif()

# Shared compiler settings — mirror what the map-server uses
if(MSVC)
    add_compile_definitions(
        _CRT_SECURE_NO_DEPRECATE
        _CRT_NONSTDC_NO_DEPRECATE
        FD_SETSIZE=4096
    )
    # Match the runtime library used by the map-server (/MD in Release, /MDd in Debug)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL$<$<CONFIG:Debug>:Debug>")
    # Suppress common warnings that are harmless for plugin code
    add_compile_options(/W3 /wd4996)
else()
    add_compile_options(-Wall -Wextra -fvisibility=hidden)
endif()

# Add each plugin subdirectory
add_subdirectory(example)

# To add your own plugin:
#   1. Create plugins/<yourplugin>/
#   2. Put your source files there
#   3. Copy example/CMakeLists.txt, rename the target and source file
#   4. add_subdirectory(<yourplugin>) here

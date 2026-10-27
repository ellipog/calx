cmake_minimum_required(VERSION 3.20)

# Release unless asked otherwise. This must come before project(), which otherwise fills in
# Debug for MSVC-style toolchains such as clang on Windows.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Debug, Release, RelWithDebInfo or MinSizeRel" FORCE)
endif()

project(calx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(calx
    src/main.cpp
    src/source.cpp
    src/lexer.cpp
    src/parser.cpp
    src/modules.cpp
    src/types.cpp
    src/check.cpp
    src/emit.cpp
    src/toolchain.cpp
)

# The runtime's sources, which the driver compiles (once, then from the cache) into every program.
# CALX_HOME at run time overrides this, for an installed compiler.
get_filename_component(CALX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
target_compile_definitions(calx PRIVATE CALX_DEFAULT_HOME="${CALX_ROOT}")

if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(calx PRIVATE /W4 /permissive-)
else()
    target_compile_options(calx PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
if(WIN32)
    target_compile_definitions(calx PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

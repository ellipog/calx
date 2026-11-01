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
    src/expand.cpp
    src/moves.cpp
    src/types.cpp
    src/check.cpp
    src/emit.cpp
    src/toolchain.cpp
    src/update.cpp
    src/doctor.cpp
)

# The runtime's sources, which the driver compiles (once, then from the cache) into every program.
# An installed compiler finds them beside itself instead, and CALX_HOME overrides both.
get_filename_component(CALX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
target_compile_definitions(calx PRIVATE CALX_DEFAULT_HOME="${CALX_ROOT}")

# The version `calx version` reports and `calx update` compares. VERSION holds the next release's
# number. CI builds a release from the tag that matches it, and a nightly with CALX_VERSION set to
# a prerelease of it, each with its commit and date. A build from source is a dev build.
file(STRINGS "${CALX_ROOT}/VERSION" CALX_BASE_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CALX_ROOT}/VERSION")
if(NOT DEFINED CALX_VERSION)
    set(CALX_VERSION "${CALX_BASE_VERSION}-dev")
endif()
if(NOT DEFINED CALX_COMMIT)
    set(CALX_COMMIT "")
endif()
if(NOT DEFINED CALX_DATE)
    set(CALX_DATE "")
endif()
target_compile_definitions(calx PRIVATE CALX_VERSION="${CALX_VERSION}" CALX_COMMIT="${CALX_COMMIT}"
                                        CALX_DATE="${CALX_DATE}")

if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(calx PRIVATE /W4 /permissive-)
else()
    target_compile_options(calx PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
if(WIN32)
    target_compile_definitions(calx PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    # A manifest that makes the driver's code page UTF-8, so its arguments and paths aren't lossy.
    # The linker merges it into the one it writes.
    target_sources(calx PRIVATE src/calx.manifest)
endif()

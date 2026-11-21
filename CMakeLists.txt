cmake_minimum_required(VERSION 3.27)
project(bloodborne_linux LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The shader translator (src/shadps4, from shadPS4) uses explicit object parameters ("deducing
# this"): GCC 14, Clang 18 or newer. Ubuntu 24.04's default GCC 13 doesn't have them.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
struct S { int v; template <class Self> auto& get(this Self& self) { return self.v; } };
int main() { S s{1}; return s.get() - 1; }" BB_HAVE_EXPLICIT_OBJECT_PARAMETERS)
if(NOT BB_HAVE_EXPLICIT_OBJECT_PARAMETERS)
    message(FATAL_ERROR "The compiler (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}) lacks "
        "explicit object parameters: use GCC 14 or Clang 18 or newer (Ubuntu 24.04: install g++-14 and "
        "configure with CC=gcc-14 CXX=g++-14 in the environment)")
endif()

# The kernel puts the heap of a non-PIE program at a random address up to 1 GiB above it, which
# can overlap the image range at loader::default_base. A PIE program and its heap go far higher.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include(CheckPIESupported)
check_pie_supported()

option(BB_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(BB_WERROR "Treat compiler warnings as errors" OFF)
option(BB_FRAME_POINTERS "Keep frame pointers, so perf can unwind with --call-graph fp" OFF)
option(BB_BUILD_TESTS "Build the unit tests" ON)
option(BB_PORTABLE "Portable package: SDL3, FFmpeg and libstdc++ built in statically" OFF)

# Warning flags for our own targets. Link it to every library and executable.
add_library(bb_warnings INTERFACE)
target_compile_options(bb_warnings INTERFACE -Wall -Wextra -Wpedantic $<$<BOOL:${BB_WERROR}>:-Werror>)

if(BB_FRAME_POINTERS)
    add_compile_options(-fno-omit-frame-pointer)
endif()

if(BB_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Checks the type of indirect calls (part of clang's -fsanitize=undefined). Game code
        # and stub thunks were not compiled with it (docs/design/runtime.md).
        add_compile_options(-fno-sanitize=function)
    endif()
    add_link_options(-fsanitize=address,undefined)
endif()

# The pipeline cache keys on the executable's build id (vk_pipeline_serialization.cpp).
add_link_options(-Wl,--build-id=sha1)

include(cmake/headers.cmake)

if(BB_PORTABLE)
    include(cmake/portable.cmake)
else()
    # A distribution without an SDL3 package (Ubuntu 24.04) builds SDL3 from its release source.
    find_package(SDL3 CONFIG QUIET)
    if(NOT SDL3_FOUND)
        message(STATUS "SDL3: no system package found, building the pinned release")
        include(cmake/sdl3.cmake)
    endif()
endif()

add_subdirectory(src)

if(BB_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    include(GoogleTest)
    add_subdirectory(tests)
endif()

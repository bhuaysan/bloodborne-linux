cmake_minimum_required(VERSION 3.27)
project(bloodborne_linux LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The kernel puts the heap of a non-PIE program at a random address up to 1 GiB above it, which
# can overlap the image range at loader::default_base. A PIE program and its heap go far higher.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include(CheckPIESupported)
check_pie_supported()

option(BB_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(BB_WERROR "Treat compiler warnings as errors" OFF)
option(BB_BUILD_TESTS "Build the unit tests" ON)

# Warning flags for our own targets. Link it to every library and executable.
add_library(bb_warnings INTERFACE)
target_compile_options(bb_warnings INTERFACE -Wall -Wextra -Wpedantic $<$<BOOL:${BB_WERROR}>:-Werror>)

if(BB_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Checks the type of indirect calls (part of clang's -fsanitize=undefined). Game code
        # and stub thunks were not compiled with it (docs/design/runtime.md).
        add_compile_options(-fno-sanitize=function)
    endif()
    add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(src)

if(BB_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    include(GoogleTest)
    add_subdirectory(tests)
endif()

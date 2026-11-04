cmake_minimum_required(VERSION 3.25)
project(bloodborne_linux LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BB_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(BB_WERROR "Treat compiler warnings as errors" OFF)
option(BB_BUILD_TESTS "Build the unit tests" ON)

# Warning flags for our own targets. Link it to every library and executable.
add_library(bb_warnings INTERFACE)
target_compile_options(bb_warnings INTERFACE -Wall -Wextra -Wpedantic $<$<BOOL:${BB_WERROR}>:-Werror>)

if(BB_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(src)

if(BB_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    include(GoogleTest)
    add_subdirectory(tests)
endif()

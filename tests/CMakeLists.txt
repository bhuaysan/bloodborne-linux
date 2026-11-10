# local.env and analysis/ exist only in the main checkout, not in linked worktrees (see
# tools/common/toolenv.py). Find it the same way, so tests behave the same in both.
execute_process(
    COMMAND git rev-parse --path-format=absolute --git-common-dir
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    OUTPUT_VARIABLE bb_git_common_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE bb_git_result
    ERROR_QUIET)
cmake_path(GET bb_git_common_dir FILENAME bb_git_common_name)
if(bb_git_result EQUAL 0 AND bb_git_common_name STREQUAL ".git")
    cmake_path(GET bb_git_common_dir PARENT_PATH BB_MAIN_CHECKOUT)
    set(BB_IN_GIT_CHECKOUT 1)
else()
    # Not a git checkout, e.g. an exported tarball.
    set(BB_MAIN_CHECKOUT ${PROJECT_SOURCE_DIR})
    set(BB_IN_GIT_CHECKOUT 0)
endif()

# Helpers shared by all tests, e.g. finding the game.
add_library(bbtestsupport STATIC support/game.cpp)
target_include_directories(bbtestsupport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(bbtestsupport PRIVATE BB_MAIN_CHECKOUT="${BB_MAIN_CHECKOUT}")
target_link_libraries(bbtestsupport PUBLIC bbcommon GTest::gtest PRIVATE bb_warnings)

# The shader corpus reader and the spirv-val wrapper, shared by tests/shadps4 and tests/gpu. The
# tests that need spirv-val skip without it.
find_program(BB_SPIRV_VAL spirv-val)
if(NOT BB_SPIRV_VAL)
    set(BB_SPIRV_VAL "")
endif()
add_library(bbcorpus STATIC support/corpus.cpp support/spirv_val.cpp)
target_include_directories(bbcorpus PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(bbcorpus PRIVATE BB_SPIRV_VAL="${BB_SPIRV_VAL}")
target_link_libraries(bbcorpus PRIVATE bb_warnings bbcommon)

# Linked into the test binaries whose tests map guest memory (support/guest_window_asan.cpp). An
# OBJECT library, so that the linker keeps its __asan_default_options.
add_library(bbtestguestwindow OBJECT support/guest_window_asan.cpp)

# bb_add_test(<name> SOURCES <files...> LIBS <targets...>) builds one test binary with gtest_main.
function(bb_add_test name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;LIBS")
    add_executable(${name} ${arg_SOURCES})
    target_link_libraries(${name} PRIVATE ${arg_LIBS} bbtestsupport GTest::gtest_main bb_warnings)
    gtest_discover_tests(${name})
endfunction()

add_subdirectory(common)
add_subdirectory(shadps4)
add_subdirectory(hle)
add_subdirectory(loader)
add_subdirectory(hook)
add_subdirectory(harness)
add_subdirectory(game)
add_subdirectory(gpu)
add_subdirectory(tools)
bb_add_test(support_test SOURCES support/game_test.cpp)
target_compile_definitions(support_test PRIVATE BB_IN_GIT_CHECKOUT=${BB_IN_GIT_CHECKOUT})

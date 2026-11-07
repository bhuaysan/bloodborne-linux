# Differential tests of the decompiled functions in bbgame against the eboot, and checks of the
# eboot addresses that the runtime names (bb-run's main-loop probe). Like
# harness_eboot_test, one binary run as one ctest test, with the harness tests' main
# (tests/harness/CMakeLists.txt).
add_executable(game_eboot_test main_loop_probe_test.cpp text_compare_test.cpp text_hash_test.cpp
                               ../harness/main.cpp)
target_link_libraries(game_eboot_test PRIVATE bbeboottest bbgame bbhle bbeboottls bb_warnings)
add_test(NAME game_eboot_test COMMAND game_eboot_test)
set_tests_properties(game_eboot_test PROPERTIES SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]")
# The clip space of the pipeline state against the eboot's state code, with the Vulkan backend.
if(TARGET bbvk)
    target_sources(game_eboot_test PRIVATE clip_space_test.cpp)
    target_link_libraries(game_eboot_test PRIVATE bbvk)
endif()

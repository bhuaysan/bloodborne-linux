# Differential tests of the decompiled functions in bbgame against the eboot. Like
# harness_eboot_test, one binary run as one ctest test, with the harness tests' main
# (tests/harness/CMakeLists.txt).
add_executable(game_eboot_test text_compare_test.cpp text_hash_test.cpp ../harness/main.cpp)
target_link_libraries(game_eboot_test PRIVATE bbeboottest bbgame bbhle bb_warnings)
add_test(NAME game_eboot_test COMMAND game_eboot_test)
set_tests_properties(game_eboot_test PROPERTIES SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]")

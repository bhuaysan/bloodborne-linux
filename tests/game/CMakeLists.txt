# Differential tests of the decompiled functions in bbgame against the eboot. Like
# harness_eboot_test, one binary run as one ctest test, with the harness tests' main
# (tests/harness/CMakeLists.txt).
add_executable(game_eboot_test ai_test.cpp aspect_test.cpp chase_cam_test.cpp chr_update_interval_test.cpp chr_validate_pace_test.cpp
                               field_of_view_test.cpp frame_test.cpp havok_test.cpp load_hold_test.cpp ragdoll_test.cpp
                               frame_buffer_test.cpp param_blend_test.cpp post_effects_test.cpp steering_test.cpp step_literals_test.cpp target_bank_test.cpp text_compare_test.cpp text_hash_test.cpp
                               ../harness/main.cpp)
target_link_libraries(game_eboot_test PRIVATE bbeboottest bbgame bbhle bbeboottls bb_warnings)
add_test(NAME game_eboot_test COMMAND game_eboot_test)
set_tests_properties(game_eboot_test PROPERTIES SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]")

# Differential tests of the decompiled functions in bbgame against the eboot. Like
# harness_eboot_test, one binary run as one ctest test (bb_add_one_entry_test), with the harness
# tests' main (tests/harness/CMakeLists.txt).
add_executable(game_eboot_test ai_test.cpp aspect_test.cpp chase_cam_test.cpp chr_update_interval_test.cpp chr_validate_pace_test.cpp
                               field_of_view_test.cpp frame_test.cpp game_options_test.cpp havok_test.cpp load_hold_test.cpp ragdoll_test.cpp
                               frame_buffer_test.cpp hud_stage_test.cpp loading_page_test.cpp param_blend_test.cpp post_effects_test.cpp steering_test.cpp step_literals_test.cpp target_bank_test.cpp text_compare_test.cpp text_hash_test.cpp
                               ../harness/main.cpp)
target_link_libraries(game_eboot_test PRIVATE bbeboottest bbgame bbgameoptions bbhle bbeboottls bb_warnings)
bb_add_one_entry_test(game_eboot_test)

# symbols/functions.csv against the hooks bbgame registers (#734). Needs no eboot.
bb_add_test(game_hook_rows_test SOURCES hook_rows_test.cpp LIBS bbgame bbhle)
target_compile_definitions(game_hook_rows_test PRIVATE BB_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

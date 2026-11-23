bb_add_test(tools_test SOURCES bb_load_test.cpp)
target_compile_definitions(tools_test PRIVATE BB_LOAD_PATH="$<TARGET_FILE:bb-load>")
add_dependencies(tools_test bb-load)
bb_add_test(bb_run_test SOURCES bb_run_test.cpp LIBS bbrunoptions bbversion)
target_compile_definitions(bb_run_test PRIVATE BB_RUN_PATH="$<TARGET_FILE:bb-run>")
add_dependencies(bb_run_test bb-run)
# bb-run's options to the renderer's (src/tools/renderer_options.hpp), where bbvideocore is built.
if(TARGET bbvideocore)
    bb_add_test(renderer_options_test SOURCES renderer_options_test.cpp
        LIBS bbrunoptions $<COMPILE_ONLY:bbvideocore>)
endif()

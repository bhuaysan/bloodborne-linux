bb_add_test(tools_test SOURCES bb_load_test.cpp)
target_compile_definitions(tools_test PRIVATE BB_LOAD_PATH="$<TARGET_FILE:bb-load>")
add_dependencies(tools_test bb-load)

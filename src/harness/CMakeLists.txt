# The differential test harness, for tests only. A STATIC library: it registers nothing, and test
# executables link the OBJECT libraries themselves (docs/design/runtime.md, "Static
# registration").
add_library(bbharness STATIC harness.cpp)
target_link_libraries(bbharness PUBLIC bbloader $<COMPILE_ONLY:bbgame> $<COMPILE_ONLY:bbhle> PRIVATE bb_warnings)

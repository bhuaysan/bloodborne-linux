# Their own main instead of gtest_main, for __asan_default_options (see main.cpp). Each binary
# links the OBJECT libraries bbgame and bbhle itself (docs/design/runtime.md, "Static
# registration").
add_executable(harness_test harness_test.cpp main.cpp)
target_link_libraries(harness_test PRIVATE bbharness bbgame bbhle bbtestsupport GTest::gtest
                                           bb_warnings)
gtest_discover_tests(harness_test)

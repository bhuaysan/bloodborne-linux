# Their own main instead of gtest_main, for __asan_default_options (see main.cpp). Each binary
# links the OBJECT libraries bbgame and bbhle itself (docs/design/runtime.md, "Static
# registration").
add_executable(harness_test harness_test.cpp main.cpp)
target_link_libraries(harness_test PRIVATE bbharness bbgame bbhle bbtestsupport GTest::gtest
                                           bb_warnings)
gtest_discover_tests(harness_test)

# The tests against the eboot, in a binary of their own: EbootTest keeps a harness for the whole
# process, and only one harness may exist at a time.
add_executable(harness_eboot_test eboot.cpp eboot_test.cpp main.cpp)
target_link_libraries(harness_eboot_test PRIVATE bbharness bbgame bbhle bbtestsupport GTest::gtest
                                                 bb_warnings)
gtest_discover_tests(harness_eboot_test)

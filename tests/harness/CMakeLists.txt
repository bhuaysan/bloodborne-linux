# Their own main instead of gtest_main, for __asan_default_options (see main.cpp). Each binary
# links the OBJECT libraries bbgame and bbhle itself (docs/design/runtime.md, "Static
# registration").
add_executable(harness_test harness_test.cpp main.cpp)
target_link_libraries(harness_test PRIVATE bbharness bbgame bbhle bbtestsupport GTest::gtest
                                           bb_warnings)
gtest_discover_tests(harness_test)

# The tests against the eboot, in a binary of their own: EbootTest keeps a harness for the whole
# process, and only one harness may exist at a time. One ctest entry runs the whole binary, so the
# eboot is loaded once per binary rather than once per test (gtest_discover_tests would start a
# process for each test).
# The EbootTest fixture, also used by the decomp tests in tests/game.
# LoadSetTest, the fixture with the PRXs loaded and initialised, lives there too.
add_library(bbeboottest STATIC eboot.cpp load_set.cpp)
target_link_libraries(bbeboottest PUBLIC bbharness bbtestsupport GTest::gtest
                                  PRIVATE $<COMPILE_ONLY:bbgame> $<COMPILE_ONLY:bbhle> bb_warnings)

add_executable(harness_eboot_test eboot_test.cpp main.cpp)
target_link_libraries(harness_eboot_test PRIVATE bbeboottest bbgame bbhle bbeboottls bb_warnings)
add_test(NAME harness_eboot_test COMMAND harness_eboot_test)
# gtest_discover_tests sets this on its entries; without it a run without the game (all cases
# skipped, gtest exits 0) would show as Passed instead of Skipped.
set_tests_properties(harness_eboot_test PROPERTIES SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]")

# The tests with the load set loaded and initialised (LoadSetTest), in a binary of their own: the
# HLE registry takes one load set per process, and EbootTest's harness would be a second one.
add_executable(harness_load_set_test load_set_test.cpp main.cpp)
target_link_libraries(harness_load_set_test PRIVATE bbeboottest bbgame bbhle bbeboottls
                                                    bb_warnings)
# Its tests map guest memory in the guest window (main.cpp).
target_compile_definitions(harness_load_set_test PRIVATE BB_TEST_GUEST_WINDOW)
add_test(NAME harness_load_set_test COMMAND harness_load_set_test)
set_tests_properties(harness_load_set_test PROPERTIES SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]")

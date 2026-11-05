# Its own main instead of gtest_main, for __asan_default_options (see main.cpp).
add_executable(harness_test harness_test.cpp main.cpp)
target_link_libraries(harness_test PRIVATE bbharness bbhle bbtestsupport GTest::gtest bb_warnings)
gtest_discover_tests(harness_test)

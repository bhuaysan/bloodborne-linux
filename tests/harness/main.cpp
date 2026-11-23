// The main of the harness tests. It also turns off AddressSanitizer's stack-use-after-return
// detection, which would move the replacements' locals off the thread's stack, so pointers to
// them would no longer compare as stack pointers (docs/design/runtime.md, "Limits for M2"). With
// BB_TEST_GUEST_WINDOW, set only for the binaries whose tests map guest memory, it also lets the
// guest window into ASan's shadow gap (tests/support/guest_window_asan.cpp); the others keep the
// gap protected.
//
// With BB_TEST_SKIP_EXIT_CODE, set for the binaries that run as one ctest entry (the eboot
// tests), a run in which nothing failed but some tests were skipped exits with that code, and
// ctest's SKIP_RETURN_CODE shows the entry as Skipped. A SKIP_REGULAR_EXPRESSION on the output
// would win over a failure: without the game every EbootTest case prints "[  SKIPPED ]", and a
// failing eboot-free test in the same binary would be reported as Skipped (#724).
#include <gtest/gtest.h>

extern "C" const char* __asan_default_options() {
#ifdef BB_TEST_GUEST_WINDOW
    return "detect_stack_use_after_return=0:protect_shadow_gap=0";
#else
    return "detect_stack_use_after_return=0";
#endif
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
#ifdef BB_TEST_SKIP_EXIT_CODE
    if (result == 0 && testing::UnitTest::GetInstance()->skipped_test_count() > 0) {
        return BB_TEST_SKIP_EXIT_CODE;
    }
#endif
    return result;
}

// The main of the harness tests. It also turns off AddressSanitizer's stack-use-after-return
// detection, which would move the replacements' locals off the thread's stack, so pointers to
// them would no longer compare as stack pointers (docs/design/runtime.md, "Limits for M2"). With
// BB_TEST_GUEST_WINDOW, set only for the binaries whose tests map guest memory, it also lets the
// guest window into ASan's shadow gap (tests/support/guest_window_asan.cpp); the others keep the
// gap protected.
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
    return RUN_ALL_TESTS();
}

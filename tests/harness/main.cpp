// The main of the harness tests. It also turns off AddressSanitizer's stack-use-after-return
// detection, which would move the replacements' locals off the thread's stack, so pointers to
// them would no longer compare as stack pointers (docs/design/runtime.md, "Limits for M2").
#include <gtest/gtest.h>

extern "C" const char* __asan_default_options() {
    return "detect_stack_use_after_return=0";
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

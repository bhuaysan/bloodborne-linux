bb_add_test(shadps4_translate_test
    SOURCES corpus_test.cpp host_test.cpp program_state.cpp signals_test.cpp
        translate_test.cpp translate.cpp
    LIBS bbshadps4 bbcorpus bbcommon)

# The renderer on the host layer, where bbvideocore is built (Vulkan headers, SDL3, glslang). It
# runs on lavapipe and skips without a Vulkan device. bbvktest holds the LeakSanitizer
# suppressions for Mesa's one-time allocations.
if(TARGET bbvideocore)
    # bbvideocore's guest memory sits on the kernel (bbhle).
    bb_add_test(shadps4_video_core_test
        SOURCES video_core_test.cpp
        LIBS bbvideocore bbhle bbvktest)
    # Maps guest memory in the guest window (tests/support/guest_window_asan.cpp).
    bb_add_test(shadps4_guest_memory_test
        SOURCES guest_memory_test.cpp
        LIBS bbvideocore bbhle bbvktest bbtestguestwindow)
endif()

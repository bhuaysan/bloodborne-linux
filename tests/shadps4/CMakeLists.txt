bb_add_test(shadps4_translate_test
    SOURCES corpus_test.cpp host_test.cpp program_state.cpp signals_test.cpp
        translate_test.cpp translate.cpp
    LIBS bbshadps4 bbcorpus bbcommon)
# The process's fault handler and the game threads' signal stacks, without a GPU.
bb_add_test(shadps4_fault_handler_test SOURCES fault_handler_test.cpp LIBS bbshadps4 bbcommon)

# The renderer on the host layer and video out's presenter over it, where bbvideocore is built
# (Vulkan headers, SDL3, glslang). It runs on lavapipe and skips without a Vulkan device. bbvktest holds the LeakSanitizer
# suppressions for Mesa's one-time allocations.
if(TARGET bbvideocore)
    # bbvideocore's guest memory sits on the kernel (bbhle).
    bb_add_test(shadps4_video_core_test
        SOURCES video_core_test.cpp
        LIBS bbvideocore bbvideopresenter bbhle bbvktest)
    # Maps guest memory in the guest window (tests/support/guest_window_asan.cpp).
    bb_add_test(shadps4_guest_memory_test
        SOURCES guest_memory_test.cpp
        LIBS bbvideocore bbhle bbvktest bbtestguestwindow)
    # Liverpool behind the GNM driver's interface, without a window; needs no Vulkan device.
    bb_add_test(shadps4_gnm_liverpool_test
        SOURCES gnm_liverpool_test.cpp
        LIBS bbvideopresenter bbvideocore bbhle bbvktest bbtestguestwindow)
endif()

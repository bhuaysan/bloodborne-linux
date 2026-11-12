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
        SOURCES video_core_test.cpp cache_storage_test.cpp
        LIBS bbvideocore bbvideopresenter bbhle bbvktest)
    # Maps guest memory in the guest window (tests/support/guest_window_asan.cpp).
    bb_add_test(shadps4_guest_memory_test
        SOURCES guest_memory_test.cpp
        LIBS bbvideocore bbhle bbvktest bbtestguestwindow)
    # Liverpool behind the GNM driver, without a window; needs no Vulkan device.
    bb_add_test(shadps4_gnm_liverpool_test
        SOURCES gnm_liverpool_test.cpp
        LIBS bbvideopresenter bbvideocore bbhle bbhlevideo bbvktest bbtestguestwindow)
    # The system libraries registered in the HLE registry, and their host pieces. Its own main
    # (see syslibs_test.cpp), so gtest and not gtest_main.
    add_executable(shadps4_syslibs_test syslibs_test.cpp)
    target_link_libraries(shadps4_syslibs_test PRIVATE bbsyslibs bbvideocore bbhle bbhlesystem
        bbvktest bbtestsupport GTest::gtest bb_warnings)
    gtest_discover_tests(shadps4_syslibs_test PROPERTIES SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]")
    # AvPlayer playing the game's opening movie, against FFmpeg's own decode (the test uses
    # FFmpeg's API itself). Skips without the game or an H.264 decoder.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(BB_TEST_FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec libavformat libavutil libswscale)
    add_executable(shadps4_avplayer_test avplayer_test.cpp)
    target_link_libraries(shadps4_avplayer_test PRIVATE bbsyslibs bbvideocore bbhle bbhlesystem
        bbvktest bbtestsupport GTest::gtest PkgConfig::BB_TEST_FFMPEG bb_warnings)
    gtest_discover_tests(shadps4_avplayer_test PROPERTIES SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]")
endif()

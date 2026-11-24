bb_add_test(shadps4_translate_test
    SOURCES corpus_test.cpp host_test.cpp program_state.cpp signals_test.cpp
        translate_test.cpp translate.cpp
    LIBS bbshadps4 bbcorpus bbcommon)
# The process's fault handler and the game threads' signal stacks, without a GPU.
bb_add_test(shadps4_fault_handler_test SOURCES fault_handler_test.cpp LIBS bbshadps4 bbcommon)
# The runtime's buffer barrier tracking (#593); header-only, needs no Vulkan device.
bb_add_test(shadps4_buffer_barriers_test SOURCES buffer_barriers_test.cpp LIBS bbshadps4 bbcommon)
# The runs of the copy shader's destination ranges (#676); header-only, needs no Vulkan device.
bb_add_test(shadps4_range_run_test SOURCES range_run_test.cpp LIBS bbshadps4 bbcommon)
# The copy shader HLE's staging layout (#832); header-only, needs no Vulkan device.
bb_add_test(shadps4_pack_copies_test SOURCES pack_copies_test.cpp LIBS bbshadps4 bbcommon)
# The registers of the pipeline lookup reuse (#780); needs no Vulkan device.
bb_add_test(shadps4_pipeline_regs_test SOURCES pipeline_regs_test.cpp LIBS bbshadps4 bbcommon)
# The anisotropy of a sampler under bb-run --anisotropy (#658); header-only, needs no Vulkan device.
bb_add_test(shadps4_sampler_anisotropy_test SOURCES sampler_anisotropy_test.cpp
    LIBS bbshadps4 bbcommon)

# The renderer on the host layer and video out's presenter over it, where bbvideocore is built
# (Vulkan headers, SDL3, glslang). It runs on lavapipe and skips without a Vulkan device. bbvktest holds the LeakSanitizer
# suppressions for Mesa's one-time allocations.
if(TARGET bbvideocore)
    # bbvideocore's guest memory sits on the kernel (bbhle).
    bb_add_test(shadps4_video_core_test
        SOURCES video_core_test.cpp cache_storage_test.cpp command_recorder_test.cpp
            pipeline_workers_test.cpp spec_reuse_test.cpp
        LIBS bbvideocore bbvideopresenter bbhle bbvktest)
    # The keyboard, text and mouse events the window hands to ImGui for the IME dialog (#795);
    # needs no window or Vulkan device.
    bb_add_test(shadps4_imgui_input_test SOURCES imgui_input_test.cpp
        LIBS bbvideocore bbhle bbvktest)
    # Maps guest memory in the guest window (tests/support/guest_window_asan.cpp).
    bb_add_test(shadps4_guest_memory_test
        SOURCES guest_memory_test.cpp
        LIBS bbvideocore bbhle bbvktest bbtestguestwindow)
    # The modules of a game run's shader cache against the corpus's translations (#512): only
    # with BB_SHADER_CACHE_DIR, the game and the extracted corpus.
    bb_add_test(shadps4_shader_cache_match_test
        SOURCES shader_cache_match_test.cpp program_state.cpp translate.cpp
        LIBS bbvideocore bbhle bbcorpus bbvktest)
    # Liverpool behind the GNM driver, without a window; needs no Vulkan device.
    bb_add_test(shadps4_gnm_liverpool_test
        SOURCES gnm_liverpool_test.cpp
        LIBS bbvideopresenter bbvideocore bbhle bbhlevideo bbvktest bbtestguestwindow)
    # The system libraries registered in the HLE registry, and their host pieces. Its own main
    # (see syslibs_test.cpp), so gtest and not gtest_main.
    add_executable(shadps4_syslibs_test syslibs_test.cpp)
    target_link_libraries(shadps4_syslibs_test PRIVATE bbsyslibs bbvideocore bbhle bbhlesystem
        bbvktest bbtestsupport GTest::gtest bb_warnings)
    gtest_discover_tests(shadps4_syslibs_test PROPERTIES SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\] ")
    # AvPlayer playing the game's opening movie, against FFmpeg's own decode (the test uses
    # FFmpeg's API itself). Skips without the game or an H.264 decoder.
    # The same FFmpeg the player links: the system's, or the portable build's own.
    if(TARGET bb_ffmpeg)
        set(BB_TEST_FFMPEG_TARGET bb_ffmpeg)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(BB_TEST_FFMPEG REQUIRED IMPORTED_TARGET
            libavcodec libavformat libavutil libswscale)
        set(BB_TEST_FFMPEG_TARGET PkgConfig::BB_TEST_FFMPEG)
    endif()
    add_executable(shadps4_avplayer_test avplayer_test.cpp)
    target_link_libraries(shadps4_avplayer_test PRIVATE bbsyslibs bbvideocore bbhle bbhlesystem
        bbvktest bbtestsupport GTest::gtest ${BB_TEST_FFMPEG_TARGET} bb_warnings)
    gtest_discover_tests(shadps4_avplayer_test PROPERTIES SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\] ")
endif()

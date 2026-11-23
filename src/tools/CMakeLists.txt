add_executable(bb-load bb_load.cpp)
target_link_libraries(bb-load PRIVATE bbhlenet bbhle bbloader bb_warnings)
# bb-run's command line, entry arguments and main-loop probe, a library of their own for the unit
# tests.
add_library(bbrunoptions STATIC run_options.cpp main_loop_probe.cpp)
target_link_libraries(bbrunoptions PUBLIC bbcommon PRIVATE bb_warnings)

# bb-run's options to the state of the game hooks and the eboot, for bb-run and the eboot tests.
# bbgame's objects come with the program that links bbgame.
add_library(bbgameoptions STATIC game_options.cpp)
target_link_libraries(bbgameoptions PUBLIC bbrunoptions PRIVATE $<COMPILE_ONLY:bbgame> bb_warnings)

# The game runtime. The OBJECT libraries hold static registrations (HLE modules, the eboot's TLS
# reservation, the replacements in bbgame), so they are linked here as objects.
add_executable(bb-run bb_run.cpp)
target_link_libraries(bb-run PRIVATE bbrunoptions bbgameoptions bbversion bbhle bbhlenet bbhlesystem bbhlevideo bbeboottls
    bbgame bbhook bbloader bbcommon bb_warnings)
# The window and video_core's presenter, where bbvideocore is built; otherwise bb-run is headless
# only.
if(TARGET bbvideopresenter)
    target_link_libraries(bb-run PRIVATE bbvideopresenter)
    target_compile_definitions(bb-run PRIVATE BB_HAVE_WINDOW)
endif()
# video_core's guest memory (src/shadps4_host/memory.cpp) and the system libraries, where
# bbvideocore is built.
if(TARGET bbvideocore)
    target_link_libraries(bb-run PRIVATE bbvideocore bbsyslibs)
    target_compile_definitions(bb-run PRIVATE BB_HAVE_VIDEO_CORE)
endif()
# --check-system needs both.
if(TARGET bbvideopresenter AND TARGET bbvideocore)
    target_sources(bb-run PRIVATE system_check.cpp)
endif()

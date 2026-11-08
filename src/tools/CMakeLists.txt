add_executable(bb-load bb_load.cpp)
target_link_libraries(bb-load PRIVATE bbhlenet bbhle bbloader bb_warnings)
add_executable(bb-gcn bb_gcn.cpp)
target_link_libraries(bb-gcn PRIVATE bbgcn bb_warnings)

# bb-run's command line, entry arguments and main-loop probe, a library of their own for the unit
# tests.
add_library(bbrunoptions STATIC run_options.cpp main_loop_probe.cpp)
target_link_libraries(bbrunoptions PUBLIC bbcommon PRIVATE bb_warnings)

# The observer that keeps the GPU memory mirror in step with direct memory (needs bbvk).
if(TARGET bbvk)
    add_library(bbmirrorobserver STATIC mirror_observer.cpp)
    target_link_libraries(bbmirrorobserver PUBLIC bbvk PRIVATE bbcommon bb_warnings)
endif()

# The game runtime. The OBJECT libraries hold static registrations (HLE modules, the eboot's TLS
# reservation, the replacements in bbgame), so they are linked here as objects.
add_executable(bb-run bb_run.cpp)
target_link_libraries(bb-run PRIVATE bbrunoptions bbhle bbhlenet bbhlesystem bbhlevideo bbeboottls
    bbgame bbhook bbloader bbcommon bb_warnings)
# The window and presentation, when SDL3 and Vulkan are there; otherwise bb-run is headless only.
if(TARGET bbwindow)
    target_link_libraries(bb-run PRIVATE bbwindow bbvk)
    target_compile_definitions(bb-run PRIVATE BB_HAVE_WINDOW)
endif()
# The GPU memory mirror, with Vulkan (also without a window).
if(TARGET bbmirrorobserver)
    target_link_libraries(bb-run PRIVATE bbmirrorobserver bbvk)
    target_compile_definitions(bb-run PRIVATE BB_HAVE_MIRROR)
endif()

# The compute and graphics runners for the execution tests: bbvk's device on lavapipe
# (docs/design/graphics.md, "Offline test strategy"). Built only when find_package(Vulkan) finds
# Vulkan (tests/gpu/).
add_library(bbcomputerunner STATIC compute_runner.cpp graphics_runner.cpp)
target_include_directories(bbcomputerunner PUBLIC ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(bbcomputerunner PUBLIC Vulkan::Vulkan bbgcn PRIVATE bbvk bb_warnings)

bb_add_test(compute_runner_test
    SOURCES compute_runner_test.cpp image_render_test.cpp memory_mirror_test.cpp render_test.cpp
            translate_exec_test.cpp
            typed_buffer_exec_test.cpp
    LIBS bbcomputerunner bbvk bbgcn bbgcntest bbvktest)

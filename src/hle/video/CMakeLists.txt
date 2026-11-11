# Video out and the GPU submit calls (src/hle/video/video.hpp, video_out.hpp), and the stage
# register imports (gnm_shader.hpp), the default state, markers and profiling queries
# (gnm_state.hpp). An OBJECT library because it holds static registrations:
# only executables link it, together with bbhle (docs/design/runtime.md, "Static registration").
# Video out is adapted from shadPS4 and uses its VideoOut types from bbshadps4's headers.
add_library(bbhlevideo OBJECT video_out.cpp gnm_driver.cpp gnm_shader.cpp gnm_state.cpp)
target_include_directories(bbhlevideo SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/amd-gcn)
target_link_libraries(bbhlevideo PUBLIC $<COMPILE_ONLY:bbhle> bbpm4 bbshadps4 PRIVATE bb_warnings)


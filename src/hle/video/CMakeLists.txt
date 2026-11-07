# Video out and the GPU submit calls (src/hle/video/video.hpp), and the stage
# register imports (gnm_shader.hpp). An OBJECT library because it holds static registrations:
# only executables link it, together with bbhle (docs/design/runtime.md, "Static registration").
add_library(bbhlevideo OBJECT video_out.cpp gnm_driver.cpp gnm_shader.cpp)
target_include_directories(bbhlevideo SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/amd-gcn)
target_link_libraries(bbhlevideo PUBLIC $<COMPILE_ONLY:bbhle> bbpm4 PRIVATE bb_warnings)

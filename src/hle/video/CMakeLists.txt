# Video out and the GPU submit calls without a GPU (src/hle/video/video.hpp). An OBJECT library
# because it holds static registrations: only executables link it, together with bbhle
# (docs/design/runtime.md, "Static registration").
add_library(bbhlevideo OBJECT video_out.cpp gnm_driver.cpp)
target_link_libraries(bbhlevideo PUBLIC $<COMPILE_ONLY:bbhle> PRIVATE bb_warnings)

# Video out and the GPU submit calls (src/hle/video/video.hpp, video_out.hpp), and the stage
# register imports (gnm_shader.hpp), the default state, markers and profiling queries
# (gnm_state.hpp). An OBJECT library because it holds static registrations:
# only executables link it, together with bbhle (docs/design/runtime.md, "Static registration").
# Video out is adapted from shadPS4 and uses its VideoOut types from bbshadps4's headers.
add_library(bbhlevideo OBJECT video_out.cpp gnm_driver.cpp gnm_shader.cpp gnm_state.cpp
    gnm_default_state.cpp)
target_include_directories(bbhlevideo SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/amd-gcn)
target_link_libraries(bbhlevideo PUBLIC $<COMPILE_ONLY:bbhle> bbshadps4 PRIVATE bb_warnings)
# The GNM driver writes the PM4 packet structs, some with 64-bit members, at any dword of a command
# buffer, as upstream does; x86-64 allows it.
set_source_files_properties(gnm_driver.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<BOOL:${BB_SANITIZE}>:-fno-sanitize=alignment>")

# video_core's presenter behind video out, for bb-run's window: Liverpool and the Presenter in
# upstream's order (video_core_presenter.hpp). Only where bbvideocore is built.
if(TARGET bbvideocore)
    add_library(bbvideopresenter STATIC video_core_presenter.cpp)
    target_link_libraries(bbvideopresenter PUBLIC bbvideocore bbshadps4 PRIVATE bbcommon bb_warnings)
    target_include_directories(bbvideopresenter PUBLIC ${PROJECT_SOURCE_DIR}/src)
endif()

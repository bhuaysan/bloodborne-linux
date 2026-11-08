# The system services (src/hle/system/system.hpp). An OBJECT library because it holds static
# registrations: only executables link it (docs/design/runtime.md, "Static registration").
add_library(bbhlesystem OBJECT app_content.cpp audio_out.cpp date_time.cpp disc_map.cpp playgo.cpp
    save_data.cpp sysmodule.cpp system.cpp system_service.cpp user_service.cpp)
target_link_libraries(bbhlesystem PUBLIC $<COMPILE_ONLY:bbhle> PRIVATE bb_warnings)

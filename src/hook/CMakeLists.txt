# Function-entry patches and the replacement registry (docs/design/runtime.md, "Hooks").
# STATIC, not OBJECT: bbhook owns the registry but registers nothing itself, so the linker may
# drop what nothing references ("Static registration").
add_library(bbhook STATIC hook.cpp)
target_link_libraries(bbhook PUBLIC bbloader PRIVATE bb_warnings)

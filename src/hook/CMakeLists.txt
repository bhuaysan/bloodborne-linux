# Function-entry patches (docs/design/runtime.md, "Hooks").
add_library(bbhook STATIC hook.cpp)
target_link_libraries(bbhook PUBLIC bbloader PRIVATE bb_warnings)

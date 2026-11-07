# bbwindow: the game window on SDL3 (docs/design/graphics.md, item 22). Built when SDL3 is found.
add_library(bbwindow STATIC window.cpp)
target_link_libraries(bbwindow PUBLIC bbcommon Vulkan::Vulkan PRIVATE SDL3::SDL3 bb_warnings)

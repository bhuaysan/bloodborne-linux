# bbvk: the Vulkan backend (docs/design/graphics.md, "Command buffers to the first frame"). So
# far the device core.
add_library(bbvk STATIC device.cpp)
target_link_libraries(bbvk PUBLIC bbcommon Vulkan::Vulkan PRIVATE bb_warnings)

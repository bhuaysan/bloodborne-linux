# bbvk: the Vulkan backend (docs/design/graphics.md, "Command buffers to the first frame"). So
# far the device core and the scheduler.
add_library(bbvk STATIC device.cpp scheduler.cpp)
target_link_libraries(bbvk PUBLIC bbcommon Vulkan::Vulkan PRIVATE bb_warnings)

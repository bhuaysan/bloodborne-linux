# bbvk: the Vulkan backend (docs/design/graphics.md, "Command buffers to the first frame"). So
# far the device core, the scheduler, the presenter, the target formats, the target image
# cache, the tiling shader and pipelines, and dynamic rendering into the targets.
add_library(bbvk STATIC device.cpp formats.cpp image_cache.cpp presenter.cpp rendering.cpp scheduler.cpp tiler.cpp)
target_link_libraries(bbvk PUBLIC bbcommon bbpm4 bbtile Vulkan::Vulkan PRIVATE bbgcn bb_warnings)

# bbvk: the Vulkan backend (docs/design/graphics.md, "Command buffers to the first frame"). So
# far the device core, the scheduler, the presenter, the target formats, the target image
# cache, the tiling shader and pipelines, dynamic rendering into the targets, the draw snapshot.
add_library(bbvk STATIC compute_pipeline.cpp device.cpp formats.cpp image_cache.cpp memory_mirror.cpp pipeline_state.cpp presenter.cpp rendering.cpp scheduler.cpp tiler.cpp)
target_link_libraries(bbvk PUBLIC bbcommon bbgcn bbpm4 bbtile Vulkan::Vulkan PRIVATE bb_warnings)

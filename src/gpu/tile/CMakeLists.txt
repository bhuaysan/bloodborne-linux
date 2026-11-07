# bbtile: tile modes and tiled surface layouts (docs/design/graphics.md, "Tiling"). Depends only on
# bbcommon, so it builds and tests without Vulkan.
add_library(bbtile STATIC tiling.cpp)
target_link_libraries(bbtile PUBLIC bbcommon PRIVATE bb_warnings)

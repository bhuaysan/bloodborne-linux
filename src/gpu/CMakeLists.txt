# Graphics libraries (docs/design/graphics.md, "Source layout").
add_subdirectory(gcn)
add_subdirectory(pm4)

# bbvk needs the Vulkan headers and loader. Without them it is not built.
find_package(Vulkan)
if(Vulkan_FOUND)
    add_subdirectory(vk)
else()
    message(STATUS "Vulkan not found: bbvk is not built")
endif()

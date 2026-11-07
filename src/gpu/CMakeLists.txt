# Graphics libraries (docs/design/graphics.md, "Source layout").
add_subdirectory(gcn)
add_subdirectory(pm4)

# bbvk needs the Vulkan headers and loader. Without them it is not built.
find_package(Vulkan)
if(Vulkan_FOUND)
    add_subdirectory(vk)
    # The game window needs SDL3 as well. Without it bb-run runs headless only.
    find_package(SDL3 CONFIG)
    if(SDL3_FOUND)
        add_subdirectory(window)
    else()
        message(STATUS "SDL3 not found: bbwindow is not built")
    endif()
else()
    message(STATUS "Vulkan not found: bbvk is not built")
endif()

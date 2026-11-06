add_subdirectory(gcn)
add_subdirectory(pm4)

# The Vulkan tests need the Vulkan headers and loader. Without them they are not built.
find_package(Vulkan)
if(Vulkan_FOUND)
    add_subdirectory(runner)
    add_subdirectory(vk)
else()
    message(STATUS "Vulkan not found: the compute runner, execution and device tests are not built")
endif()

add_subdirectory(gcn)

# The execution tests need the Vulkan headers and loader. Without them they are not built.
find_package(Vulkan)
if(Vulkan_FOUND)
    add_subdirectory(runner)
else()
    message(STATUS "Vulkan not found: the compute runner and execution tests are not built")
endif()

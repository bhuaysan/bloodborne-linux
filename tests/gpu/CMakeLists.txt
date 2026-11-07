add_subdirectory(gcn)
add_subdirectory(pm4)
add_subdirectory(tile)

# The Vulkan tests need the Vulkan headers and loader. Without them they are not built.
find_package(Vulkan)
if(Vulkan_FOUND)
    # Linked into every Vulkan test binary: the LeakSanitizer suppressions for Mesa.
    add_library(bbvktest OBJECT lsan_vulkan.cpp)
    target_link_libraries(bbvktest PRIVATE bb_warnings)
    add_subdirectory(runner)
    add_subdirectory(vk)
    if(TARGET bbwindow)
        add_subdirectory(window)
    endif()
else()
    message(STATUS "Vulkan not found: the compute runner, execution and device tests are not built")
endif()

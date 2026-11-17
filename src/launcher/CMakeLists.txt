# bb-launcher (docs/design/launcher.md). Built where SDL3, fontconfig and the Vulkan headers are
# found. The pure logic is a library of its own for the unit tests (tests/launcher).
add_library(bblauncherlogic STATIC logic.cpp text.cpp)
target_include_directories(bblauncherlogic PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bblauncherlogic PUBLIC bbcommon PRIVATE bb_warnings)

find_package(SDL3 CONFIG QUIET)
find_package(Vulkan QUIET)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(BB_FONTCONFIG QUIET IMPORTED_TARGET fontconfig)
endif()
if(NOT (SDL3_FOUND AND Vulkan_FOUND AND BB_FONTCONFIG_FOUND))
    message(STATUS "SDL3, the Vulkan headers or fontconfig not found: bb-launcher is not built")
    return()
endif()

# Its own ImGui at the default config, from the sources that shadps4_host fetched. bb_imgui has
# shadPS4's config, which changes ImTextureID and the index size.
FetchContent_GetProperties(imgui_src SOURCE_DIR BB_LAUNCHER_IMGUI_DIR)
add_library(bb_launcher_imgui STATIC
    ${BB_LAUNCHER_IMGUI_DIR}/imgui.cpp ${BB_LAUNCHER_IMGUI_DIR}/imgui_draw.cpp
    ${BB_LAUNCHER_IMGUI_DIR}/imgui_tables.cpp ${BB_LAUNCHER_IMGUI_DIR}/imgui_widgets.cpp
    ${BB_LAUNCHER_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${BB_LAUNCHER_IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(bb_launcher_imgui SYSTEM PUBLIC ${BB_LAUNCHER_IMGUI_DIR})
target_link_libraries(bb_launcher_imgui PUBLIC SDL3::SDL3)
target_compile_options(bb_launcher_imgui PRIVATE -w)

# The window's parts: widgets, the main screen, fonts, the GPU probe, the header picture and the
# pages. One file per page: a new page_*.cpp is picked up without editing this list.
file(GLOB BB_LAUNCHER_PAGES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/page_*.cpp)
add_library(bblauncherui STATIC main_screen.cpp widgets.cpp fonts.cpp gpu_probe.cpp
    header_image.cpp stb_impl.cpp ${BB_LAUNCHER_PAGES})
target_include_directories(bblauncherui PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_include_directories(bblauncherui SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/stb)
set_source_files_properties(stb_impl.cpp PROPERTIES COMPILE_OPTIONS -w)
target_link_libraries(bblauncherui PUBLIC bblauncherlogic bb_launcher_imgui
    PRIVATE PkgConfig::BB_FONTCONFIG Vulkan::Headers ${CMAKE_DL_LIBS} bb_warnings)

add_executable(bb-launcher main.cpp app.cpp)
target_link_libraries(bb-launcher PRIVATE bblauncherui bb_warnings)

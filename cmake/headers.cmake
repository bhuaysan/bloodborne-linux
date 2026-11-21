# Vulkan-Headers and SPIRV-Headers: the system's when they are new enough, else pinned copies
# fetched from the Khronos releases (as cmake/sdl3.cmake does for SDL3), so a distribution with older
# headers (Ubuntu 24.04, Debian 13) still builds. The pins are the versions of Fedora 44, which the
# code is written against. The message says which were used.
#
# BB_FETCH_HEADERS: auto (default, fetch when the system's are too old or missing), always (for a
# build image whose headers should not matter, e.g. the portable package's) or never (a missing or
# old system header then fails where it is first used).
include_guard(GLOBAL)
include(FetchContent)
include(CheckCXXSourceCompiles)

set(BB_FETCH_HEADERS auto CACHE STRING "Use fetched Vulkan and SPIR-V headers: auto, always or never")
set_property(CACHE BB_FETCH_HEADERS PROPERTY STRINGS auto always never)
if(NOT BB_FETCH_HEADERS MATCHES "^(auto|always|never)$")
    message(FATAL_ERROR "BB_FETCH_HEADERS must be auto, always or never, not '${BB_FETCH_HEADERS}'")
endif()

# What the code needs of Vulkan, through the newest names it uses: VK_KHR_shader_maximal_reconvergence
# (absent from Ubuntu 24.04's headers) and the Mesa KosmicKrisp driver id, which Debian 13's
# Vulkan-Hpp (1.4.309) lacks. A newer header also brings the Vulkan-Hpp calls that changed
# their return type, as seen on Debian 13 (freeDescriptorSets).
set(BB_VULKAN_CHECK_SOURCE "
#include <vulkan/vulkan.hpp>
int main() {
    return VK_KHR_SHADER_MAXIMAL_RECONVERGENCE_EXTENSION_NAME[0] + int(vk::DriverId::eMesaKosmickrisp);
}")
# What the shader translator and sirit need of SPIR-V.
set(BB_SPIRV_CHECK_SOURCE "
#include <spirv/unified1/spirv.hpp11>
int main() { return static_cast<int>(spv::ExecutionMode::MaximallyReconvergesKHR); }")

function(bb_system_header_version header_name var)
    find_path(dir ${header_name} NO_CACHE)
    set(${var} "" PARENT_SCOPE)
    if(dir)
        # The version the message names: VK_HEADER_VERSION, or SPIR-V's revision.
        file(STRINGS ${dir}/${header_name} line REGEX "define (VK_HEADER_VERSION|SPV_REVISION) +[0-9]+" LIMIT_COUNT 1)
        if(line MATCHES "([0-9]+)$")
            set(${var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
        else()
            set(${var} "unknown" PARENT_SCOPE)
        endif()
    endif()
endfunction()

# Vulkan.
bb_system_header_version(vulkan/vulkan_core.h bb_vk_found)
set(bb_use_fetched_vulkan FALSE)
if(BB_FETCH_HEADERS STREQUAL "always")
    set(bb_use_fetched_vulkan TRUE)
elseif(BB_FETCH_HEADERS STREQUAL "auto")
    check_cxx_source_compiles("${BB_VULKAN_CHECK_SOURCE}" BB_SYSTEM_VULKAN_HEADERS_OK)
    if(NOT BB_SYSTEM_VULKAN_HEADERS_OK)
        set(bb_use_fetched_vulkan TRUE)
    endif()
endif()
if(bb_use_fetched_vulkan)
    if(bb_vk_found)
        message(STATUS "Vulkan headers: system copy (VK_HEADER_VERSION ${bb_vk_found}) is too old "
            "or not wanted, fetching Vulkan-Headers 1.4.341")
    else()
        message(STATUS "Vulkan headers: none installed, fetching Vulkan-Headers 1.4.341")
    endif()
    FetchContent_Declare(VulkanHeaders
        URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.341.tar.gz
        URL_HASH SHA256=8876aad926b2e72bdefddc34885472d5332e2d20aa3ed57313466f0bc982cf3f
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        SYSTEM EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(VulkanHeaders)
    # FindVulkan (still run for the loader library) keeps this target instead of making its own.
    if(NOT TARGET Vulkan::Headers)
        add_library(Vulkan::Headers ALIAS Vulkan-Headers)
    endif()
else()
    message(STATUS "Vulkan headers: system copy (VK_HEADER_VERSION ${bb_vk_found})")
endif()

# SPIR-V.
find_package(SPIRV-Headers CONFIG QUIET)
set(bb_use_fetched_spirv FALSE)
if(BB_FETCH_HEADERS STREQUAL "always" OR (BB_FETCH_HEADERS STREQUAL "auto" AND NOT SPIRV-Headers_FOUND))
    set(bb_use_fetched_spirv TRUE)
elseif(BB_FETCH_HEADERS STREQUAL "auto")
    get_target_property(bb_spv_dirs SPIRV-Headers::SPIRV-Headers INTERFACE_INCLUDE_DIRECTORIES)
    set(CMAKE_REQUIRED_INCLUDES ${bb_spv_dirs})
    check_cxx_source_compiles("${BB_SPIRV_CHECK_SOURCE}" BB_SYSTEM_SPIRV_HEADERS_OK)
    unset(CMAKE_REQUIRED_INCLUDES)
    if(NOT BB_SYSTEM_SPIRV_HEADERS_OK)
        set(bb_use_fetched_spirv TRUE)
    endif()
endif()
if(bb_use_fetched_spirv)
    bb_system_header_version(spirv/unified1/spirv.h bb_spv_found)
    if(bb_spv_found)
        message(STATUS "SPIR-V headers: system copy (SPV_REVISION ${bb_spv_found}) is too old or "
            "not wanted, fetching SPIRV-Headers 29981f65")
    else()
        message(STATUS "SPIR-V headers: none installed, fetching SPIRV-Headers 29981f65")
    endif()
    set(SPIRV_HEADERS_SKIP_EXAMPLES ON CACHE BOOL "" FORCE)
    set(SPIRV_HEADERS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(SPIRV-Headers
        URL https://github.com/KhronosGroup/SPIRV-Headers/archive/29981f65241605e08b0ede4cfeb999fe3b723c6a.tar.gz
        URL_HASH SHA256=232899f1ad4104fb5bc377b94596c7621575eee62ad9a9e8f929b63a7dd8a7ad
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        SYSTEM EXCLUDE_FROM_ALL
        OVERRIDE_FIND_PACKAGE)
    FetchContent_MakeAvailable(SPIRV-Headers)
else()
    message(STATUS "SPIR-V headers: system copy")
endif()

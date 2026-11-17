# Unit tests of the launcher's pure logic, and runs of bb-launcher itself on SDL's offscreen video
# driver. New *_test.cpp files are picked up without editing this list.
file(GLOB BB_LAUNCHER_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
if(NOT TARGET bblauncherui)
    list(FILTER BB_LAUNCHER_TESTS EXCLUDE REGEX "ui_test\\.cpp$")
endif()
bb_add_test(launcher_test SOURCES ${BB_LAUNCHER_TESTS} LIBS bblauncherlogic bbtestsupport)
if(TARGET bblauncherui)
    target_link_libraries(launcher_test PRIVATE bblauncherui)
endif()
if(TARGET bb-launcher)
    target_compile_definitions(launcher_test PRIVATE BB_LAUNCHER_PATH="$<TARGET_FILE:bb-launcher>")
    add_dependencies(launcher_test bb-launcher)
endif()

# Unit tests of the launcher's pure logic and of its window parts (tests/launcher/ui_test.cpp,
# built where SDL3 is found). New *_test.cpp files are picked up without editing this list.
file(GLOB BB_LAUNCHER_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
if(NOT TARGET bblauncherui)
    list(FILTER BB_LAUNCHER_TESTS EXCLUDE REGEX "ui_test\\.cpp$")
endif()
bb_add_test(launcher_test SOURCES ${BB_LAUNCHER_TESTS} LIBS bblauncherlogic bbtestsupport)
if(TARGET bblauncherui)
    target_link_libraries(launcher_test PRIVATE bblauncherui)
endif()

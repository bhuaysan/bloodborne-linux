# Unit tests of the launcher's pure logic. New *_test.cpp files are picked up without editing this
# list.
file(GLOB BB_LAUNCHER_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
bb_add_test(launcher_test SOURCES ${BB_LAUNCHER_TESTS} LIBS bblauncherlogic bbtestsupport)

# bb-launcher (docs/design/launcher.md). The pure logic is a library of its own for the unit tests
# (tests/launcher).
add_library(bblauncherlogic STATIC logic.cpp text.cpp)
target_include_directories(bblauncherlogic PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bblauncherlogic PUBLIC bbcommon PRIVATE bb_warnings)

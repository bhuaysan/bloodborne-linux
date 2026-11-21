add_library(bbcommon STATIC clock.cpp config.cpp game_folder.cpp env.cpp file.cpp frame_profile.cpp log.cpp memory.cpp pad_stream.cpp runtime_dirs.cpp sha256.cpp
    signal_stack.cpp)
target_include_directories(bbcommon PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bbcommon PRIVATE bb_warnings)
# RuntimeDirs refuses directories inside the checkout the runtime was built from.
set_source_files_properties(runtime_dirs.cpp PROPERTIES COMPILE_DEFINITIONS
    "BB_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
# Every eboot load hashes the whole file. Unoptimized and with sanitizers, that is most of the
# load time of the debug build, so there this one file is optimized and uninstrumented.
set_source_files_properties(sha256.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CONFIG:Debug>:-O2>;$<$<BOOL:${BB_SANITIZE}>:-fno-sanitize=all>")

# The build's version (common/version.hpp), a library of its own so that a new commit relinks
# only the programs that show it. The custom target runs at every build; cmake/version.cmake
# rewrites the file only when the version changed. BB_VERSION overrides it for builds without git.
set(BB_VERSION "" CACHE STRING "Version to build in instead of git describe (docs/releasing.md)")
set(BB_VERSION_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/version.cpp)
add_custom_target(bb_version_source
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -DOUTPUT=${BB_VERSION_SOURCE}
        -DVERSION=${BB_VERSION} -P ${PROJECT_SOURCE_DIR}/cmake/version.cmake
    BYPRODUCTS ${BB_VERSION_SOURCE}
    COMMENT "Checking the build version"
    VERBATIM)
add_library(bbversion STATIC ${BB_VERSION_SOURCE})
add_dependencies(bbversion bb_version_source)
target_include_directories(bbversion PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bbversion PRIVATE bb_warnings)

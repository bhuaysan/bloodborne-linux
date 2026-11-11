add_library(bbcommon STATIC clock.cpp code_hash.cpp env.cpp file.cpp log.cpp memory.cpp runtime_dirs.cpp sha256.cpp
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

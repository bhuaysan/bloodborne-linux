add_library(bbcommon STATIC env.cpp file.cpp memory.cpp sha256.cpp)
target_include_directories(bbcommon PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bbcommon PRIVATE bb_warnings)
# Every eboot load hashes the whole file. Unoptimized and with sanitizers, that is most of the
# load time of the debug build, so there this one file is optimized and uninstrumented.
set_source_files_properties(sha256.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CONFIG:Debug>:-O2>;$<$<BOOL:${BB_SANITIZE}>:-fno-sanitize=all>")

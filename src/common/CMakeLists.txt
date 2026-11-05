add_library(bbcommon STATIC env.cpp file.cpp sha256.cpp)
target_include_directories(bbcommon PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bbcommon PRIVATE bb_warnings)
# Every eboot load hashes the whole file. Unoptimized and with sanitizers, that is most of the
# load time of the debug build, so this one file is optimized and uninstrumented in every preset.
set_source_files_properties(sha256.cpp PROPERTIES COMPILE_OPTIONS "-O2;-fno-sanitize=all")

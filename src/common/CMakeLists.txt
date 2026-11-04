add_library(bbcommon STATIC env.cpp sha256.cpp)
target_include_directories(bbcommon PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(bbcommon PRIVATE bb_warnings)

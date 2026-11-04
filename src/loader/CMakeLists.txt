add_library(bbloader STATIC elf.cpp)
target_link_libraries(bbloader PUBLIC bbcommon PRIVATE bb_warnings)

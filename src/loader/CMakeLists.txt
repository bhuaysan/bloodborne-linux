add_library(bbloader STATIC dynamic.cpp elf.cpp image.cpp self.cpp)
target_link_libraries(bbloader PUBLIC bbcommon PRIVATE bb_warnings)

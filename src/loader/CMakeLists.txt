add_library(bbloader STATIC dynamic.cpp elf.cpp image.cpp load_set.cpp relocate.cpp self.cpp
            unwind.cpp)
target_link_libraries(bbloader PUBLIC bbcommon PRIVATE bb_warnings)

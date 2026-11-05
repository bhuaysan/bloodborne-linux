# bbgcn: GCN shader decoder, disassembler and (later) translator. Depends only on bbcommon.
add_library(bbgcn STATIC
    decode.cpp
    opcodes.cpp)
target_link_libraries(bbgcn PUBLIC bbcommon PRIVATE bb_warnings)

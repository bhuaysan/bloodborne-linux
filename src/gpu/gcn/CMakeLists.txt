# bbgcn: GCN shader decoder, disassembler, SPIR-V builder and (later) translator. Depends only on
# bbcommon and the Khronos SPIR-V headers (spirv-headers-devel / spirv-headers).
find_path(BB_SPIRV_INCLUDE_DIR spirv/unified1/spirv.hpp11 REQUIRED)

add_library(bbgcn STATIC
    buffer_format.cpp
    cfg.cpp
    alu.cpp
    descriptors.cpp
    decode.cpp
    disassemble.cpp
    ds_flat.cpp
    fetch.cpp
    lds.cpp
    memory.cpp
    opcodes.cpp
    spirv_builder.cpp
    stages.cpp
    structurize.cpp
    translate.cpp
    vop3_decode.cpp
    vop_decode.cpp
    vop_opcodes.cpp)
target_include_directories(bbgcn SYSTEM PUBLIC ${BB_SPIRV_INCLUDE_DIR})
target_link_libraries(bbgcn PUBLIC bbcommon PRIVATE bb_warnings)

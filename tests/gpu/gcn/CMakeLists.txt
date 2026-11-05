# llvm-mc assembles the round-trip tests' source, and spirv-val validates the SPIR-V builder's
# modules. The tests that need either skip without it.
find_program(BB_LLVM_MC llvm-mc)
if(NOT BB_LLVM_MC)
    set(BB_LLVM_MC "")
endif()
find_program(BB_SPIRV_VAL spirv-val)
if(NOT BB_SPIRV_VAL)
    set(BB_SPIRV_VAL "")
endif()

bb_add_test(gcn_test
    SOURCES decode_test.cpp llvm_mc.cpp roundtrip_test.cpp spirv_builder_test.cpp spirv_val.cpp
    LIBS bbgcn)
target_compile_definitions(gcn_test PRIVATE BB_LLVM_MC="${BB_LLVM_MC}"
                                            BB_SPIRV_VAL="${BB_SPIRV_VAL}")

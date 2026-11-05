# llvm-mc assembles the round-trip tests' source; they skip without it (it isn't in CI).
find_program(BB_LLVM_MC llvm-mc)
if(NOT BB_LLVM_MC)
    set(BB_LLVM_MC "")
endif()

bb_add_test(gcn_test
    SOURCES decode_test.cpp llvm_mc.cpp roundtrip_test.cpp spirv_builder_test.cpp LIBS bbgcn)
target_compile_definitions(gcn_test PRIVATE BB_LLVM_MC="${BB_LLVM_MC}")

# llvm-mc assembles the round-trip tests' source and checks the test programs' words, and
# spirv-val validates the SPIR-V builder's modules. The tests that need either skip without it.
find_program(BB_LLVM_MC llvm-mc)
if(NOT BB_LLVM_MC)
    set(BB_LLVM_MC "")
endif()
find_program(BB_SPIRV_VAL spirv-val)
if(NOT BB_SPIRV_VAL)
    set(BB_SPIRV_VAL "")
endif()

# Helpers shared with the execution tests: llvm-mc, spirv-val and the test programs.
add_library(bbgcntest STATIC llvm_mc.cpp programs.cpp spirv_val.cpp)
target_include_directories(bbgcntest PUBLIC ${PROJECT_SOURCE_DIR}/tests)
target_compile_definitions(bbgcntest PRIVATE BB_LLVM_MC="${BB_LLVM_MC}"
                                             BB_SPIRV_VAL="${BB_SPIRV_VAL}"
                                             BB_GCN_PROGRAMS_DIR="${PROJECT_SOURCE_DIR}/tests/gpu/programs")
target_link_libraries(bbgcntest PRIVATE bb_warnings)

bb_add_test(gcn_test
    SOURCES cfg_test.cpp decode_test.cpp disassemble_test.cpp ds_flat_test.cpp memory_test.cpp
            programs_test.cpp roundtrip_test.cpp spirv_builder_test.cpp structurize_test.cpp
    LIBS bbgcn bbgcntest)

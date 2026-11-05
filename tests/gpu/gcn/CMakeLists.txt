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

# The llvm-mc and spirv-val helpers, shared with the execution tests.
add_library(bbgcntest STATIC llvm_mc.cpp spirv_val.cpp)
target_include_directories(bbgcntest PUBLIC ${PROJECT_SOURCE_DIR}/tests)
target_compile_definitions(bbgcntest PRIVATE BB_LLVM_MC="${BB_LLVM_MC}"
                                             BB_SPIRV_VAL="${BB_SPIRV_VAL}")
target_link_libraries(bbgcntest PRIVATE bb_warnings)

bb_add_test(gcn_test
    SOURCES cfg_test.cpp decode_test.cpp roundtrip_test.cpp spirv_builder_test.cpp
    LIBS bbgcn bbgcntest)

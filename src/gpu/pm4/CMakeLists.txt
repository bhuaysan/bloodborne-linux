# bbpm4: PM4 packet headers, the GPU register mirror, the parser and the render target state.
# Depends only on bbcommon and AMD's register and opcode headers in third_party/amd-gcn.
add_library(bbpm4 STATIC registers.cpp parser.cpp targets.cpp gpu_thread.cpp
            default_state.cpp)
target_include_directories(bbpm4 SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/amd-gcn)
target_link_libraries(bbpm4 PUBLIC bbcommon PRIVATE bb_warnings)

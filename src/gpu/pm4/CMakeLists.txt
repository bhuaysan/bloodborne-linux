# bbpm4: PM4 packet headers, the GPU register mirror and the render target state (the parser
# follows). Depends only on bbcommon and AMD's register and opcode headers in third_party/amd-gcn.
add_library(bbpm4 STATIC registers.cpp targets.cpp)
target_include_directories(bbpm4 SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/amd-gcn)
target_link_libraries(bbpm4 PUBLIC bbcommon PRIVATE bb_warnings)

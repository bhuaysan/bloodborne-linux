# The network and PSN libraries, reported as offline (docs/design/m3-core-os.md, "Network and
# PSN"). An OBJECT library because it registers HLE functions: only executables link it, together
# with bbhle (docs/design/runtime.md, "Static registration").
add_library(bbhlenet OBJECT net.cpp)
target_link_libraries(bbhlenet PUBLIC $<COMPILE_ONLY:bbhle> PRIVATE bb_warnings)

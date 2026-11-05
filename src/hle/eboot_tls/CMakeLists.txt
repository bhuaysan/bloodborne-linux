# The reservation for the eboot's static TLS block (docs/design/m3-core-os.md, "TLS: one thread
# pointer for game and runtime"). An OBJECT library because it must be part of the executable's own
# TLS segment and registers itself at static initialization: bb-run and every test executable that
# runs game code link it, together with bbhle.
add_library(bbeboottls OBJECT eboot_tls.S eboot_tls.cpp)
target_link_libraries(bbeboottls PUBLIC $<COMPILE_ONLY:bbhle> PRIVATE bb_warnings)

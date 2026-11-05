# Decompiled functions and the image accessor (docs/design/runtime.md, "Calling the game"). An
# OBJECT library because the replacements in it register themselves: only executables link it
# ("Static registration").
add_library(bbgame OBJECT image.cpp text/compare.cpp text/hash.cpp)
target_link_libraries(bbgame PUBLIC bbhook $<COMPILE_ONLY:bbhle> PRIVATE bb_warnings)

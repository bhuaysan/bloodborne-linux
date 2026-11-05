# An OBJECT library because it holds static registrations: only executables link it
# (docs/design/runtime.md, "Static registration").
add_library(bbhle OBJECT hle.cpp stub_entry.S)
target_link_libraries(bbhle PUBLIC bbloader PRIVATE bb_warnings)

# The NID name table, generated from symbols/nids.csv (docs/design/runtime.md, "Function stubs").
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(BB_NIDS_CSV ${PROJECT_SOURCE_DIR}/symbols/nids.csv)
set(BB_NID_TABLE_SCRIPT ${PROJECT_SOURCE_DIR}/tools/nidtable/gen_nid_table.py)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/nid_table.cpp
    COMMAND Python3::Interpreter ${BB_NID_TABLE_SCRIPT} ${BB_NIDS_CSV}
            ${CMAKE_CURRENT_BINARY_DIR}/nid_table.cpp
    DEPENDS ${BB_NIDS_CSV} ${BB_NID_TABLE_SCRIPT}
    COMMENT "Generating the NID name table"
    VERBATIM)

# The kernel modules (docs/design/m3-core-os.md): every source file in kernel/, so that the
# M3 issues, which each add one, don't all edit the same list.
file(GLOB BB_HLE_KERNEL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/kernel/*.cpp)

# An OBJECT library because it holds static registrations: only executables link it
# (docs/design/runtime.md, "Static registration").
add_library(bbhle OBJECT hle.cpp link.cpp stub_entry.S ${CMAKE_CURRENT_BINARY_DIR}/nid_table.cpp
    ${BB_HLE_KERNEL_SOURCES})
target_link_libraries(bbhle PUBLIC bbloader PRIVATE bb_warnings)
add_subdirectory(net)
add_subdirectory(system)

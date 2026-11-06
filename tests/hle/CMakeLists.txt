bb_add_test(hle_test SOURCES hle_test.cpp handle_table_test.cpp LIBS bbhle)
bb_add_test(hle_net_test SOURCES net_test.cpp LIBS bbhlenet bbhle)
bb_add_test(hle_system_test SOURCES system_test.cpp LIBS bbhle bbhlesystem)
bb_add_test(hle_date_time_test SOURCES date_time_test.cpp LIBS bbhle bbhlesystem)
bb_add_test(hle_save_data_test SOURCES save_data_test.cpp LIBS bbhle bbhlesystem)
# The stage register imports check their layouts against the shader files with the decoder.
bb_add_test(hle_video_test SOURCES video_test.cpp gnm_shader_test.cpp LIBS bbhle bbhlevideo bbgcn)
# Runs the PRX initialisers, so it needs the eboot's TLS reservation (docs/design/m3-core-os.md,
# "TLS").
bb_add_test(hle_prx_init_test SOURCES prx_init_test.cpp LIBS bbhle bbeboottls bbtestguestwindow)

# One test executable per kernel module: kernel_<module>_test.cpp becomes the test
# kernel_<module>_test, so that new kernel modules don't all edit this list.
file(GLOB kernel_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/kernel_*_test.cpp)
foreach(source IN LISTS kernel_tests)
    get_filename_component(name ${source} NAME_WE)
    bb_add_test(${name} SOURCES ${source} LIBS bbhle)
endforeach()
# The TLS tests check the eboot's reservation too (docs/design/m3-core-os.md, "TLS").
target_link_libraries(kernel_tls_test PRIVATE bbeboottls)
# New threads run the eboot TLS check.
target_link_libraries(kernel_thread_test PRIVATE bbeboottls)
target_link_libraries(kernel_thread_extras_test PRIVATE bbeboottls)
target_link_libraries(kernel_thread_specific_test PRIVATE bbeboottls)
# Their tests map guest memory in the guest window (tests/support/guest_window_asan.cpp).
foreach(name IN ITEMS kernel_vmem_test kernel_dmem_test kernel_mmap_file_test)
    target_link_libraries(${name} PRIVATE bbtestguestwindow)
endforeach()

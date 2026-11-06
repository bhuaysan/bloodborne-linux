bb_add_test(hle_test SOURCES hle_test.cpp LIBS bbhle)
bb_add_test(hle_net_test SOURCES net_test.cpp LIBS bbhlenet bbhle)
bb_add_test(hle_system_test SOURCES system_test.cpp LIBS bbhle bbhlesystem)
bb_add_test(hle_date_time_test SOURCES date_time_test.cpp LIBS bbhle bbhlesystem)
bb_add_test(hle_save_data_test SOURCES save_data_test.cpp LIBS bbhle bbhlesystem)
bb_add_test(hle_video_test SOURCES video_test.cpp LIBS bbhle bbhlevideo)
# Runs the PRX initialisers, so it needs the eboot's TLS reservation (docs/design/m3-core-os.md,
# "TLS").
bb_add_test(hle_prx_init_test SOURCES prx_init_test.cpp LIBS bbhle bbeboottls)

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

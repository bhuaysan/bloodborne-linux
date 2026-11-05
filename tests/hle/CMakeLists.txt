bb_add_test(hle_test SOURCES hle_test.cpp LIBS bbhle)
bb_add_test(hle_net_test SOURCES net_test.cpp LIBS bbhlenet bbhle)
bb_add_test(kernel_abi_test SOURCES kernel_abi_test.cpp LIBS bbhle)
bb_add_test(hle_system_test SOURCES system_test.cpp LIBS bbhle bbhlesystem)

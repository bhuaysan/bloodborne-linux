bb_add_test(vk_test
    SOURCES compute_pipeline_test.cpp device_test.cpp formats_test.cpp image_cache_test.cpp memory_mirror_test.cpp
        pipeline_state_test.cpp presenter_test.cpp rendering_test.cpp scheduler_test.cpp
        tiler_test.cpp
    LIBS bbvk bbmirrorobserver bbgcntest bbvktest)

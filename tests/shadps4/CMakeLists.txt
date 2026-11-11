bb_add_test(shadps4_translate_test
    SOURCES corpus_test.cpp program_state.cpp signals_test.cpp
        translate_test.cpp translate.cpp
    LIBS bbshadps4 bbcorpus bbcommon)

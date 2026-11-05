add_executable(bb-load bb_load.cpp)
target_link_libraries(bb-load PRIVATE bbhle bbloader bb_warnings)

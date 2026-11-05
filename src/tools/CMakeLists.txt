add_executable(bb-load bb_load.cpp)
target_link_libraries(bb-load PRIVATE bbhlenet bbhle bbloader bb_warnings)

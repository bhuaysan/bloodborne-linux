add_executable(bb-load bb_load.cpp)
target_link_libraries(bb-load PRIVATE bbhlenet bbhle bbloader bb_warnings)
add_executable(bb-gcn bb_gcn.cpp)
target_link_libraries(bb-gcn PRIVATE bbgcn bb_warnings)

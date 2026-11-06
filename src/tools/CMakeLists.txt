add_executable(bb-load bb_load.cpp)
target_link_libraries(bb-load PRIVATE bbhlenet bbhle bbloader bb_warnings)
add_executable(bb-gcn bb_gcn.cpp)
target_link_libraries(bb-gcn PRIVATE bbgcn bb_warnings)

# bb-run's command line and entry arguments, a library of their own for the unit tests.
add_library(bbrunoptions STATIC run_options.cpp)
target_link_libraries(bbrunoptions PUBLIC bbcommon PRIVATE bb_warnings)

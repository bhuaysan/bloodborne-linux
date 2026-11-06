add_executable(bb-load bb_load.cpp)
target_link_libraries(bb-load PRIVATE bbhlenet bbhle bbloader bb_warnings)
add_executable(bb-gcn bb_gcn.cpp)
target_link_libraries(bb-gcn PRIVATE bbgcn bb_warnings)

# bb-run's command line and entry arguments, a library of their own for the unit tests.
add_library(bbrunoptions STATIC run_options.cpp)
target_link_libraries(bbrunoptions PUBLIC bbcommon PRIVATE bb_warnings)

# The game runtime. The OBJECT libraries hold static registrations (HLE modules, the eboot's TLS
# reservation, the replacements in bbgame), so they are linked here as objects.
add_executable(bb-run bb_run.cpp)
target_link_libraries(bb-run PRIVATE bbrunoptions bbhle bbhlenet bbhlesystem bbhlevideo bbeboottls
    bbgame bbhook bbloader bbcommon bb_warnings)

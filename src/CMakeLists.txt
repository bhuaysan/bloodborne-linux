# Libraries and their dependencies are described in docs/design/runtime.md.
add_subdirectory(common)
add_subdirectory(loader)
# Before hle: src/hle/video builds its presenter only where bbvideocore exists.
add_subdirectory(shadps4_host)
add_subdirectory(hle)
add_subdirectory(hook)
add_subdirectory(game)
add_subdirectory(harness)
add_subdirectory(tools)

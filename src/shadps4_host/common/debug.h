// shadPS4's Tracy profiler markers as no-ops: the port doesn't build Tracy.
#pragma once

#define TRACY_GPU_ENABLED 0

// The one marker colour video_core names (in its Tracy zones, compiled out).
enum MarkersPalette : int { GpuMarkerColor = 0xf4a261 };

#define RENDERER_TRACE
#define TRACE_HINT(str) static_cast<void>(str)
#define FRAME_END
#define FIBER_ENTER(name)
#define FIBER_EXIT

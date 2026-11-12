// shadPS4's Tracy profiler markers as no-ops: the port doesn't build Tracy.
#pragma once

#define RENDERER_TRACE
#define TRACE_HINT(str) static_cast<void>(str)
#define FRAME_END
#define FIBER_ENTER(name)
#define FIBER_EXIT

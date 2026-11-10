// Tracy's Vulkan profiler interface as no-ops: the port doesn't build Tracy. The context is a null
// pointer, and the zones that use it are compiled out with TRACY_GPU_ENABLED 0 (common/debug.h).
#pragma once

namespace tracy {
class VkCtxScope;
}  // namespace tracy

using TracyVkCtx = void*;

#define TracyVkContextHostCalibrated(...) nullptr
#define TracyVkContextName(ctx, name, size)
#define TracyVkNamedZoneC(...)
#define TracyVkCollect(ctx, cmdbuf)

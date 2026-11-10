// The developer tools' GPU memory viewer, always disabled: the port has no developer tools.
#pragma once

#include <string>
#include <vector>

#include "common/types.h"

namespace Core::Devtools::Widget {

struct GpuMemoryRow {
    std::string name;
    u64 bytes{};
};

struct GpuMemoryGroup {
    std::string name;
    std::vector<GpuMemoryRow> rows;
};

class GpuMemoryViewer {
public:
    static constexpr bool IsEnabled() { return false; }
    static void Publish(GpuMemoryGroup) {}
};

}  // namespace Core::Devtools::Widget

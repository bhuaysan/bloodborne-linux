// The functions video_core and the ImGui renderer core expect from the rest of shadPS4, for
// bbvideocore (docs/design/renderer.md). Fixed no-ops where the port leaves a feature out, and
// host_missing() where a later R4 issue provides the real thing, so that reaching one is loud.

#include <string>

#include "common/log.hpp"
#include "shadps4_host/failure.hpp"

// IM_ASSERT of shadPS4's ImGui config (imgui/imgui_config.h).
void assert_fail_debug_msg(const char* msg) {
    bb::common::log_fatal("shadps4", std::string("ImGui assertion failed: ") + msg);
}

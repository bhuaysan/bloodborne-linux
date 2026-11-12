// What is left of shadPS4's debug state: the one member vk_pipeline_cache.cpp still calls. It goes
// with that call.
#pragma once

namespace DebugStateType {

class DebugStateImpl {
public:
    // EmulatorSettings keeps shader collection off, so this is not called.
    void CollectShader(const auto&...) {}
};

}  // namespace DebugStateType

inline DebugStateType::DebugStateImpl DebugState;

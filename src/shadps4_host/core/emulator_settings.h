// What the translator reads of shadPS4's settings object, fixed at the values this port runs with.
// A port for one game has nothing to configure here. DMA and inlined fetch shaders stay off, as
// they are by default in the v.0.21.0 reference.
#pragma once

namespace bb::shader {

struct FixedSettings {
    static constexpr bool IsDumpShaders() { return false; }
    static constexpr bool IsDirectMemoryAccessEnabled() { return false; }
    static constexpr bool IsInlineFetchShader() { return false; }
};

}  // namespace bb::shader

inline constexpr bb::shader::FixedSettings EmulatorSettings{};

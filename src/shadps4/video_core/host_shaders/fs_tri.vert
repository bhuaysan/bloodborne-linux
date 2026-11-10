// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/host_shaders/fs_tri.vert @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#version 450

#if defined(INSTANCE_AS_LAYER)
#extension GL_ARB_shader_viewport_layer_array : require
#endif

layout(location = 0) out vec2 uv;

void main() {
    vec2 pos = vec2(
        float((gl_VertexIndex & 1u) << 2u),
        float((gl_VertexIndex & 2u) << 1u)
    );
    gl_Position = vec4(pos - vec2(1.0, 1.0), 0.0, 1.0);
#if defined(INSTANCE_AS_LAYER)
    gl_Layer = gl_InstanceIndex;
#endif
    uv = pos * 0.5;
}

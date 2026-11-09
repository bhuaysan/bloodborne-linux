// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Source: shadPS4 src/shader_recompiler/backend/spirv/emit_spirv_quad_rect.cpp (EmitRectListTCS),
// commit 0d5f2739d2544d2a35c73e354cba7a32b7a81bea (docs/references.md).
// Changed for bloodborne-linux, 2026-11-09: a geometry stage written with our SPIR-V builder
// instead of a tessellation control shader in sirit; it emits a four-vertex triangle strip.

#include "gpu/gcn/rect_list.hpp"

#include <array>

#include "gpu/gcn/spirv_builder.hpp"

namespace bb::gcn {

namespace {
using Id = SpirvBuilder::Id;
using SC = spv::StorageClass;
}  // namespace

std::vector<std::uint32_t> rect_list_geometry(std::span<const std::uint32_t> locations) {
    SpirvBuilder b;
    b.capability(spv::Capability::Shader);
    b.capability(spv::Capability::Geometry);
    b.memory_model(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);

    const Id void_t = b.type_void(), bool_t = b.type_bool(), f32 = b.type_float(32);
    const Id i32 = b.type_int(32, true), vec2 = b.type_vector(f32, 2),
             vec4 = b.type_vector(f32, 4), bvec2 = b.type_vector(bool_t, 2);
    const auto index = [&](std::int32_t n) { return b.constant_i32(n); };
    const auto word = [](auto value) { return spirv_word(value); };

    // gl_in[3].gl_Position and gl_Position, each as the one member of a gl_PerVertex block.
    std::vector<Id> interface;
    const auto block = [&] {
        const Id type = b.type_struct({vec4});
        b.decorate(type, spv::Decoration::Block);
        b.member_decorate(type, 0, spv::Decoration::BuiltIn, {word(spv::BuiltIn::Position)});
        return type;
    };
    const Id in_block = block(), out_block = block();
    const Id in_array = b.type_array(in_block, b.constant_u32(3));
    const Id gl_in = b.variable(b.type_pointer(SC::Input, in_array), SC::Input);
    const Id gl_out = b.variable(b.type_pointer(SC::Output, out_block), SC::Output);
    interface.push_back(gl_in);
    interface.push_back(gl_out);

    // One vec4[3] input and one vec4 output per location.
    const Id in_vec4s = b.type_array(vec4, b.constant_u32(3));
    std::vector<Id> params_in, params_out;
    for (const std::uint32_t location : locations) {
        const Id in = b.variable(b.type_pointer(SC::Input, in_vec4s), SC::Input);
        const Id out = b.variable(b.type_pointer(SC::Output, vec4), SC::Output);
        b.decorate(in, spv::Decoration::Location, {location});
        b.decorate(out, spv::Decoration::Location, {location});
        params_in.push_back(in);
        params_out.push_back(out);
        interface.push_back(in);
        interface.push_back(out);
    }

    const Id main = b.begin_function(void_t, b.type_function(void_t));
    b.begin_block(b.id());
    const Id in_vec4 = b.type_pointer(SC::Input, vec4), out_vec4 = b.type_pointer(SC::Output, vec4);
    const auto load = [&](Id type, Id pointer) { return b.op(spv::Op::OpLoad, type, {pointer}); };
    const auto chain = [&](Id type, Id base, std::vector<Id> at) {
        at.insert(at.begin(), base);
        return b.op(spv::Op::OpAccessChain, type, at);
    };
    const auto position = [&](std::int32_t i) {
        return load(vec4, chain(in_vec4, gl_in, {index(i), index(0)}));
    };

    // The corner: eq[i] says which coordinates vertex i shares with vertex i + 1. A vertex is the
    // corner when it shares x with its successor and y with its predecessor, or the other way.
    std::array<Id, 3> xy, eq;
    for (int i = 0; i < 3; ++i) {
        const Id p = position(i);
        xy[i] = b.op(spv::Op::OpVectorShuffle, vec2, {p, p, 0, 1});
    }
    for (int i = 0; i < 3; ++i) {
        eq[i] = b.op(spv::Op::OpFOrdEqual, bvec2, {xy[i], xy[(i + 1) % 3]});
    }
    const auto lane = [&](Id v, std::uint32_t n) {
        return b.op(spv::Op::OpCompositeExtract, bool_t, {v, n});
    };
    std::array<Id, 3> corner, weight;
    for (int i = 0; i < 3; ++i) {
        const Id previous = eq[(i + 2) % 3];
        const Id xy_corner = b.op(spv::Op::OpLogicalAnd, bool_t, {lane(eq[i], 0), lane(previous, 1)});
        const Id yx_corner = b.op(spv::Op::OpLogicalAnd, bool_t, {lane(eq[i], 1), lane(previous, 0)});
        corner[i] = b.op(spv::Op::OpLogicalOr, bool_t, {xy_corner, yx_corner});
        weight[i] = b.op(spv::Op::OpSelect, f32, {corner[i], b.constant_f32(-1), b.constant_f32(1)});
    }
    // The corner's index (0 when there is none), where the strip starts.
    Id first = b.op(spv::Op::OpSelect, i32, {corner[2], index(2), index(0)});
    first = b.op(spv::Op::OpSelect, i32, {corner[1], index(1), first});

    const auto fourth = [&](const std::array<Id, 3>& v) {
        Id sum = 0;
        for (int i = 0; i < 3; ++i) {
            const Id term = b.op(spv::Op::OpVectorTimesScalar, vec4, {v[i], weight[i]});
            sum = sum == 0 ? term : b.op(spv::Op::OpFAdd, vec4, {sum, term});
        }
        return sum;
    };
    // Vertex k of the strip is input (first + k) % 3, the fourth is the derived one. The strip
    // (corner, next, previous, fourth) covers the rectangle as two triangles.
    const auto emit = [&](Id pos, const std::vector<Id>& values) {
        b.op_void(spv::Op::OpStore, {chain(out_vec4, gl_out, {index(0)}), pos});
        for (std::size_t n = 0; n < values.size(); ++n) {
            b.op_void(spv::Op::OpStore, {params_out[n], values[n]});
        }
        b.op_void(spv::Op::OpEmitVertex);
    };
    std::array<Id, 3> positions;
    for (int i = 0; i < 3; ++i) {
        positions[i] = position(i);
    }
    std::vector<std::array<Id, 3>> attributes;
    for (const Id in : params_in) {
        attributes.push_back({load(vec4, chain(in_vec4, in, {index(0)})),
                              load(vec4, chain(in_vec4, in, {index(1)})),
                              load(vec4, chain(in_vec4, in, {index(2)}))});
    }
    for (int k = 0; k < 3; ++k) {
        const Id at = b.op(spv::Op::OpSMod, i32, {b.op(spv::Op::OpIAdd, i32, {first, index(k)}), index(3)});
        std::vector<Id> values;
        for (const Id in : params_in) {
            values.push_back(load(vec4, chain(in_vec4, in, {at})));
        }
        emit(load(vec4, chain(in_vec4, gl_in, {at, index(0)})), values);
    }
    std::vector<Id> values;
    for (const auto& a : attributes) {
        values.push_back(fourth(a));
    }
    emit(fourth(positions), values);
    b.op_void(spv::Op::OpEndPrimitive);
    b.op_void(spv::Op::OpReturn);
    b.end_function();

    b.entry_point(spv::ExecutionModel::Geometry, main, "main", interface);
    b.execution_mode(main, spv::ExecutionMode::Triangles);
    b.execution_mode(main, spv::ExecutionMode::OutputTriangleStrip);
    b.execution_mode(main, spv::ExecutionMode::OutputVertices, {4});
    b.execution_mode(main, spv::ExecutionMode::Invocations, {1});
    return b.words();
}

}  // namespace bb::gcn

// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/shader_recompiler/backend/spirv/emit_spirv_image.cpp @0d5f2739d2544d2a35c73e354cba7a32b7a81bea
// Changed for bloodborne-linux, 2026-11-06: only the cube coordinates of FixImageCoords (a cube
// as a 2D array), in our SPIR-V builder.
//
// The sample instructions follow the Sea Islands ISA manual, 8.2.3 and 8.2.4 (the address: the
// derivatives, then x, y and the slice or face_id, then the LOD) and 8.2.5 (the T# type field).

#include "gpu/gcn/image.hpp"

#include <algorithm>
#include <format>
#include <iterator>
#include <string_view>

#include <spirv/unified1/GLSL.std.450.h>

#include "gpu/gcn/buffer_format.hpp"

namespace bb::gcn {

ImageClass image_class(std::span<const std::uint32_t, 4> tsharp) {
    const unsigned number = tsharp[1] >> 26 & 0xf;
    return {.type = static_cast<std::uint8_t>(tsharp[3] >> 28),
            .number = number == nfmt::uint_  ? ImageClass::Number::uint
                      : number == nfmt::sint ? ImageClass::Number::sint
                                             : ImageClass::Number::float_};
}

namespace detail {

namespace {

using Op = spv::Op;

constexpr std::uint8_t type_2d = 9, type_cube = 11, type_2d_array = 13;

// How a sample instruction gets its LOD.
enum class Lod { implicit, explicit_, zero, gradient };

struct SampleForm {
    std::string_view name;
    Lod lod;
};

constexpr SampleForm sample_forms[] = {{"image_sample", Lod::implicit},
                                       {"image_sample_l", Lod::explicit_},
                                       {"image_sample_lz", Lod::zero},
                                       {"image_sample_d", Lod::gradient}};

}  // namespace

void ImageOps::translate(const Instruction& inst, std::uint32_t index) {
    const auto* form = std::ranges::find(sample_forms, inst.name, &SampleForm::name);
    if (form == std::end(sample_forms)) {
        regs_.fail(std::format("unsupported instruction {}", inst.name));
    }
    if ((inst.flags & (flag::tfe | flag::lwe | flag::unorm)) != 0) {
        regs_.fail(std::format("{} with tfe, lwe or unorm is not supported yet", inst.name));
    }
    if (form->lod == Lod::implicit && key_.stage != Stage::pixel) {
        regs_.fail(std::format("{} (implicit LOD) outside the pixel stage", inst.name));
    }
    const auto use = sources_.uses.find(index);
    if (use == sources_.uses.end()) {
        regs_.fail(std::format("the image or sampler descriptor of {} can't be followed",
                               inst.name));
    }
    const std::uint32_t i = use->second.image;
    const ImageClass c = i < key_.images.size() ? key_.images[i] : ImageClass{};
    if (c.type != type_2d && c.type != type_cube && c.type != type_2d_array) {
        regs_.fail(std::format("image type {} of {} is not supported yet", c.type, inst.name));
    }

    // The address, one float per VGPR from vaddr (its decoded range is the shortest form).
    unsigned next = inst.src[0].code;
    const auto take = [&] {
        return b_.op(Op::OpBitcast, f32(), {b_.op(Op::OpLoad, u32(), {regs_.vgpr(next++)})});
    };
    const auto vec = [&](const SpirvBuilder::Words& parts) {
        const auto n = static_cast<std::uint32_t>(parts.size());
        return b_.op(Op::OpCompositeConstruct, b_.type_vector(f32(), n), parts);
    };
    Id dx = 0, dy = 0;
    if (form->lod == Lod::gradient) {  // dx/dh, dy/dh, dx/dv, dy/dv
        const Id d0 = take(), d1 = take(), d2 = take(), d3 = take();
        dx = vec({d0, d1});
        dy = vec({d2, d3});
    }
    const Id x = take(), y = take();
    Id coords;
    if (c.type == type_cube) {
        // The cube math leaves x and y in [1, 2], and face_id is slice * 8 + face: the face's
        // layer of the cube as a 2D array is face_id - 2 * floor(face_id / 8).
        const Id face = take();
        const Id glsl = b_.import_extended("GLSL.std.450");
        const Id one = b_.constant_f32(1.0f);
        const Id group = b_.op(Op::OpExtInst, f32(),
                               {glsl, GLSLstd450Floor,
                                b_.op(Op::OpFMul, f32(), {face, b_.constant_f32(0.125f)})});
        const Id layer = b_.op(Op::OpExtInst, f32(),
                               {glsl, GLSLstd450Fma, group, b_.constant_f32(-2.0f), face});
        coords = vec({b_.op(Op::OpFSub, f32(), {x, one}), b_.op(Op::OpFSub, f32(), {y, one}),
                      layer});
    } else {
        // With da the address has a slice, which only an array uses; an array without it reads
        // its first layer (8.2.5).
        const Id slice = (inst.flags & flag::da) != 0 ? take() : b_.constant_f32(0.0f);
        coords = c.type == type_2d_array ? vec({x, y, slice}) : vec({x, y});
    }

    const Id component = c.number == ImageClass::Number::float_
                             ? f32()
                             : b_.type_int(32, c.number == ImageClass::Number::sint);
    const Id image_t = b_.type_image(component, spv::Dim::Dim2D, 0, c.type != type_2d, false, 1);
    const Id sampler_t = b_.type_sampler();
    const auto first_sampler = static_cast<std::uint32_t>(sources_.images.size());
    SpirvBuilder::Words operands = {
        b_.op(Op::OpSampledImage, b_.type_sampled_image(image_t),
              {b_.op(Op::OpLoad, image_t, {binding(image_t, i)}),
               b_.op(Op::OpLoad, sampler_t,
                     {binding(sampler_t, first_sampler + *use->second.sampler)})}),
        coords};
    if (form->lod == Lod::gradient) {
        operands.insert(operands.end(), {spirv_word(spv::ImageOperandsMask::Grad), dx, dy});
    } else if (form->lod != Lod::implicit) {
        operands.insert(operands.end(), {spirv_word(spv::ImageOperandsMask::Lod),
                                         form->lod == Lod::zero ? b_.constant_f32(0.0f) : take()});
    }
    const Id texel = b_.op(form->lod == Lod::implicit ? Op::OpImageSampleImplicitLod
                                                      : Op::OpImageSampleExplicitLod,
                           b_.type_vector(component, 4), operands);

    // dmask picks the components into consecutive VGPRs; with dmask 0 the one VGPR gets 0.
    unsigned dst = inst.dst[0].code;
    const unsigned dmask = inst.mem_offset;
    if (dmask == 0) {
        regs_.write_vgpr(dst, b_.constant_u32(0));
    }
    for (std::uint32_t k = 0; k < 4; ++k) {
        if ((dmask >> k & 1) != 0) {
            Id value = b_.op(Op::OpCompositeExtract, component, {texel, k});
            if (c.number != ImageClass::Number::uint) {
                value = b_.op(Op::OpBitcast, u32(), {value});
            }
            regs_.write_vgpr(dst++, value);
        }
    }
}

ImageOps::Id ImageOps::binding(Id type, std::uint32_t index) {
    using SC = spv::StorageClass;
    auto [it, added] = bindings_.try_emplace(index, 0);
    if (added) {
        it->second = b_.variable(b_.type_pointer(SC::UniformConstant, type), SC::UniformConstant);
        b_.decorate(it->second, spv::Decoration::DescriptorSet, {descriptor_set(key_.stage)});
        b_.decorate(it->second, spv::Decoration::Binding, {index});
        interface_.push_back(it->second);
    }
    return it->second;
}

}  // namespace detail

}  // namespace bb::gcn

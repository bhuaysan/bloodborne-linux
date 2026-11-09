// MIMG sampling (docs/design/graphics.md, "Image sampling"): the image and sampler bindings and
// the sample instructions. Internal to the translator.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "gpu/gcn/descriptors.hpp"
#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/spirv_builder.hpp"
#include "gpu/gcn/stages.hpp"
#include "gpu/gcn/translate.hpp"

namespace bb::gcn::detail {

class ImageOps {
public:
    using Id = SpirvBuilder::Id;

    ImageOps(SpirvBuilder& b, const ShaderKey& key, StageRegisters& regs,
             const ImageSources& sources)
        : b_(b), key_(key), regs_(regs), sources_(sources) {}

    // The MIMG instruction at `index` of the program. Throws GcnError for the ones that aren't
    // supported.
    void translate(const Instruction& inst, std::uint32_t index);

    // The image and sampler variables declared so far, for the entry point.
    const std::vector<Id>& interface() const { return interface_; }

private:
    // image_store and image_load_mip.
    void no_sampler(const Instruction& inst, std::uint32_t index);
    // The components of `texel`, a vec4 of `component`, that dmask selects into dst[0]'s VGPRs.
    void write_result(const Instruction& inst, ImageClass c, Id component, Id texel);
    Id binding(Id type, std::uint32_t index);  // the UniformConstant variable at that binding
    Id f32() { return b_.type_float(32); }
    Id u32() { return b_.type_int(32, false); }

    SpirvBuilder& b_;
    const ShaderKey& key_;
    StageRegisters& regs_;
    const ImageSources& sources_;
    std::vector<Id> interface_;
    std::map<std::uint32_t, Id> bindings_;  // by binding
};

}  // namespace bb::gcn::detail

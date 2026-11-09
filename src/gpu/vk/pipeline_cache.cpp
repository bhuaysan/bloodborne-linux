#include "gpu/vk/pipeline_cache.hpp"

#include <format>
#include <optional>
#include <string_view>
#include <utility>

#include "common/log.hpp"
#include "gpu/gcn/gcn.hpp"
#include "gpu/vk/device.hpp"

namespace bb::vk {

namespace {

std::uint64_t hash_stages(const PipelineKey& key) {
    const std::uint64_t ps = key.ps ? key.ps->code_address : 0;
    return key.vs.code_address ^ ps * 0x9e3779b97f4a7c15u;
}

void destroy(VkDevice device, GraphicsPipeline& p) {
    vkDestroyPipeline(device, p.pipeline, nullptr);
    vkDestroyPipelineLayout(device, p.layout, nullptr);
}

constexpr std::string_view missing_fetch = "without a fetch subroutine";  // gcn/fetch.cpp

}  // namespace

PipelineCache::PipelineCache(const Device& device, CodeReader read)
    : device_(device), read_(std::move(read)) {}

PipelineCache::~PipelineCache() {
    for (auto& [hash, entry] : entries_) {
        destroy(device_.handle(), entry.pipeline);
    }
}

bool PipelineCache::fetchless(const StageState& vs, std::span<const std::uint32_t> code) const {
    const auto [first, last] = fetchless_.equal_range(vs.code_address);
    for (auto it = first; it != last; ++it) {
        if (it->second.key == vs.key && it->second.code.matches(code)) {
            return true;
        }
    }
    return false;
}

const GraphicsPipeline* PipelineCache::get(const PipelineKey& request,
                                           const RenderingInfo& targets, std::string& why) {
    Entry entry{request,
                {targets.color, targets.color_count, targets.depth, targets.stencil,
                 targets.samples},
                {},
                {},
                {},
                {},
                {}};
    PipelineKey& key = entry.key;
    for (std::uint32_t n = targets.color_count; n < key.blend.size(); ++n) {
        key.blend[n] = {};  // no such attachment: not part of the pipeline
    }
    // The code is read again on every request: an entry counts only while the code it was made
    // from is still there. Older entries stay (the GPU may still use their pipelines) and
    // match again if their code returns.
    const auto vs_now = read_(key.vs.code_address);
    if (fetchless(key.vs, vs_now)) {
        key.vs.fetch_address = 0;
    }
    const auto ps_now = key.ps ? read_(key.ps->code_address) : std::span<const std::uint32_t>{};
    const auto fetch_now = key.vs.fetch_address != 0 ? read_(key.vs.fetch_address)
                                                      : std::span<const std::uint32_t>{};
    const auto found = [&]() -> std::optional<const GraphicsPipeline*> {
        const auto [first, last] = entries_.equal_range(hash_stages(key));
        for (auto it = first; it != last; ++it) {
            const Entry& e = it->second;
            if (e.key == key && e.formats == entry.formats && e.vs_code.matches(vs_now) &&
                e.ps_code.matches(ps_now) && e.fetch_code.matches(fetch_now)) {
                why = e.error;
                return e.pipeline.pipeline != VK_NULL_HANDLE ? &e.pipeline : nullptr;
            }
        }
        return std::nullopt;
    };
    if (const auto hit = found()) {
        return *hit;
    }

    // Unreadable code isn't kept: the program may be mapped by the next request.
    // A stage that fails to translate keeps the stamps of the code it read (the extent of a
    // rejected program is unknown).
    const auto translate = [&](const StageState& s, std::span<const std::uint32_t> code,
                               std::span<const std::uint32_t> fetch, common::CodeStamp& stamp,
                               common::CodeStamp& fetch_stamp) -> std::optional<gcn::Translation> {
        if (code.empty()) {
            why = std::format("no readable code at {:#x}", s.code_address);
            return std::nullopt;
        }
        ++translations_;
        stamp = common::CodeStamp::of(code, common::error_stamp_dwords);
        fetch_stamp = common::CodeStamp::of(fetch, common::error_stamp_dwords);
        try {
            auto t = gcn::translate(code, fetch, s.key);
            stamp = common::CodeStamp::of(code, t.code_dwords);
            fetch_stamp = common::CodeStamp::of(fetch, t.fetch_dwords);
            return t;
        } catch (const gcn::GcnError& e) {
            // A VS that needs the fetch subroutine and finds it unmapped may succeed once it is.
            if (s.fetch_address != 0 && fetch.empty() &&
                std::string_view(e.what()).find(missing_fetch) != std::string_view::npos) {
                why = std::format("no readable code at {:#x}", s.fetch_address);
                return std::nullopt;
            }
            throw;
        }
    };
    std::optional<gcn::Translation> vs, ps;
    try {
        common::CodeStamp no_fetch;
        if (!(vs = translate(key.vs, vs_now, fetch_now, entry.vs_code, entry.fetch_code))) {
            return nullptr;
        }
        // Before the first translation of this VS no entry holds it, so no entry has to be
        // looked up again without the fetch address.
        if (!vs->interface.calls_fetch && !fetchless(key.vs, vs_now)) {
            fetchless_.emplace(key.vs.code_address, Fetchless{key.vs.key, entry.vs_code});
            key.vs.fetch_address = 0;
        }
        if (key.ps && !(ps = translate(*key.ps, ps_now, {}, entry.ps_code, no_fetch))) {
            return nullptr;
        }
        // An MIMG instruction takes a T#, so a stage with samplers has images too.
        if (!vs->interface.images.empty() || (ps && !ps->interface.images.empty())) {
            entry.error = "images and samplers are not supported yet";
        }
    } catch (const gcn::GcnError& e) {
        entry.error = std::format("the {} at {:#x} doesn't translate: {}", vs ? "PS" : "VS",
                                  vs ? key.ps->code_address : key.vs.code_address, e.what());
        if (!vs && key.vs.fetch_address != 0) {
            // A VS error that doesn't depend on the fetch subroutine is kept without its
            // address, so a per-draw pointer in the user data doesn't add an entry per draw.
            try {
                gcn::translate(vs_now, {}, key.vs.key);
            } catch (const gcn::GcnError& probe) {
                if (std::string_view(probe.what()) == e.what()) {
                    fetchless_.emplace(key.vs.code_address, Fetchless{key.vs.key, entry.vs_code});
                    key.vs.fetch_address = 0;
                    entry.fetch_code = {};
                }
            }
        }
    }
    why = entry.error;
    const auto node = entries_.emplace(hash_stages(key), std::move(entry));
    Entry& stored = node->second;
    if (!why.empty()) {
        common::log(common::LogLevel::warn, "gpu.vk", "{}", why);
        return nullptr;
    }
    // A failing Vulkan call may be transient (out of memory): drop the entry and what it
    // created, so the next request tries again.
    try {
        create(stored, *vs, ps ? &*ps : nullptr);
    } catch (...) {
        destroy(device_.handle(), stored.pipeline);
        entries_.erase(node);
        throw;
    }
    return &stored.pipeline;
}

void PipelineCache::create(Entry& entry, const gcn::Translation& vs, const gcn::Translation* ps) {
    const VkDevice device = device_.handle();
    const PipelineKey& key = entry.key;
    const Formats& f = entry.formats;
    GraphicsPipeline& p = entry.pipeline;
    struct Modules {
        VkDevice device;
        VkShaderModule handles[2] = {};
        ~Modules() {
            for (VkShaderModule m : handles) {
                vkDestroyShaderModule(device, m, nullptr);
            }
        }
    } modules{device};
    p.vs = vs.interface;
    p.ps = ps ? ps->interface : gcn::ShaderInterface{};
    const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                    0, gcn::graphics_push_size};
    auto layout = make<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(device, &layout, nullptr, &p.layout), "vkCreatePipelineLayout");
    if (create_hook_) create_hook_();

    VkPipelineShaderStageCreateInfo stages[2];
    const gcn::Translation* translations[2] = {&vs, ps};
    const std::uint32_t stage_count = ps ? 2 : 1;
    for (std::uint32_t i = 0; i < stage_count; ++i) {
        auto module = make<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        module.codeSize = translations[i]->spirv.size() * sizeof(std::uint32_t);
        module.pCode = translations[i]->spirv.data();
        check(vkCreateShaderModule(device, &module, nullptr, &modules.handles[i]),
              "vkCreateShaderModule");
        stages[i] = make<VkPipelineShaderStageCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        stages[i].stage = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[i].module = modules.handles[i];
        stages[i].pName = "main";
    }

    // No vertex buffers: the translated VS fetches through the mirror.
    const auto vertex_input = make<VkPipelineVertexInputStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    auto assembly = make<VkPipelineInputAssemblyStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    assembly.topology = key.topology;
    auto clip = make<VkPipelineViewportDepthClipControlCreateInfoEXT>(
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT);
    clip.negativeOneToOne = key.negative_one_to_one;
    auto viewport = make<VkPipelineViewportStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, &clip);
    viewport.viewportCount = viewport.scissorCount = 1;
    auto raster = make<VkPipelineRasterizationStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
    raster.depthClampEnable = key.depth_clamp;
    raster.polygonMode = key.polygon_mode;
    raster.lineWidth = 1;
    auto multisample = make<VkPipelineMultisampleStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    multisample.rasterizationSamples = static_cast<VkSampleCountFlagBits>(f.samples);
    const auto depth_stencil = make<VkPipelineDepthStencilStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
    // The blend state of the key per colour target; the write mask nibble is VkColorComponentFlags.
    VkPipelineColorBlendAttachmentState attachments[pm4::color_targets] = {};
    last_blend_enable_ = 0;
    for (std::uint32_t n = 0; n < pm4::color_targets; ++n) {
        const BlendAttachment& b = key.blend[n];
        // A format without COLOR_ATTACHMENT_BLEND (integer formats) can't blend: the hardware
        // writes the shader's value there, as with blending off.
        bool can_blend = false;
        if (b.enable && n < f.color_count) {
            VkFormatProperties properties;
            vkGetPhysicalDeviceFormatProperties(device_.physical(), f.color[n], &properties);
            can_blend = (properties.optimalTilingFeatures &
                         VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0;
        }
        last_blend_enable_ |= std::uint32_t{can_blend} << n;
        attachments[n] = {can_blend,    b.src_color, b.dst_color, b.color_op,
                          b.src_alpha, b.dst_alpha, b.alpha_op,  b.write_mask};
    }
    auto blend = make<VkPipelineColorBlendStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    blend.attachmentCount = f.color_count;
    blend.pAttachments = attachments;
    auto dynamic = make<VkPipelineDynamicStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
    dynamic.dynamicStateCount = std::size(graphics_dynamic_states);
    dynamic.pDynamicStates = graphics_dynamic_states;
    auto rendering = make<VkPipelineRenderingCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
    rendering.colorAttachmentCount = f.color_count;
    rendering.pColorAttachmentFormats = f.color.data();
    rendering.depthAttachmentFormat = f.depth;
    rendering.stencilAttachmentFormat = f.stencil;

    auto info = make<VkGraphicsPipelineCreateInfo>(
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering);
    info.stageCount = stage_count;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth_stencil;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = p.layout;
    check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &p.pipeline),
          "vkCreateGraphicsPipelines");
}

}  // namespace bb::vk

// Records the draws of the PM4 parser on the scheduler (docs/design/graphics.md, item 27): the
// pipeline of the draw snapshot, a draw block per stage, the dynamic state, the index buffer
// from the memory mirror, and draw or drawIndexed inside the dynamic rendering of the targets.
#pragma once

#include <atomic>
#include <cstdint>
#include <set>
#include <string>

#include <vulkan/vulkan.h>

#include "gpu/pm4/parser.hpp"
#include "gpu/pm4/targets.hpp"

namespace bb::vk {

class Device;
class Dispatcher;
class MemoryMirror;
class PipelineCache;
class Scheduler;
class TargetRenderer;
class TextureBinder;

// Draws by outcome: recorded, not recorded because the state or a shader isn't supported yet (or
// no target is bound), or not recorded because of an error (a Vulkan failure, an index buffer
// outside the mirror or misaligned).
struct DrawStats {
    std::uint64_t recorded = 0;
    std::uint64_t unsupported = 0;
    std::uint64_t failed = 0;
};

// Used by one thread (the GPU thread), with the scheduler and the dispatcher, whose draw blocks
// and sync points it shares. Everything it is given must outlive it. `textures` binds the images
// and samplers of the stages; without it a draw whose shaders sample counts as unsupported.
class DrawRecorder {
public:
    DrawRecorder(const Device& device, Scheduler& scheduler, Dispatcher& dispatcher,
                 PipelineCache& pipelines, TargetRenderer& targets, const MemoryMirror& mirror,
                 const pm4::ExtentHints& hints, TextureBinder* textures = nullptr)
        : device_(device), scheduler_(scheduler), dispatcher_(dispatcher), pipelines_(pipelines),
          targets_(targets), mirror_(mirror), hints_(hints), textures_(textures) {}

    // Records one draw with the registers `regs`. Never throws: a draw that can't be recorded is
    // counted and logged (reasons once each). Indirect draws are not supported yet.
    void draw(const pm4::RegisterMirror& regs, const pm4::DrawCall& call);

    // A snapshot; another thread may read it while the GPU thread draws.
    DrawStats stats() const { return {recorded_.load(), unsupported_.load(), failed_.load()}; }

private:
    // Returns whether the draw was recorded; otherwise `why` says what isn't supported.
    bool record(const pm4::RegisterMirror& regs, const pm4::DrawCall& call, std::string& why);

    const Device& device_;
    Scheduler& scheduler_;
    Dispatcher& dispatcher_;
    PipelineCache& pipelines_;
    TargetRenderer& targets_;
    const MemoryMirror& mirror_;
    const pm4::ExtentHints& hints_;
    TextureBinder* textures_;
    std::atomic<std::uint64_t> recorded_ = 0, unsupported_ = 0, failed_ = 0;
    std::set<std::string> logged_;  // the reasons already logged
};

}  // namespace bb::vk

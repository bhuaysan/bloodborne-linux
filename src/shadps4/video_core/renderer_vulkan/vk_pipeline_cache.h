// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/renderer_vulkan/vk_pipeline_cache.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-14
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <optional>
#include <string_view>

#include <tsl/robin_map.h>
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_pipeline_workers.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "vulkan/vulkan.hpp"

namespace AmdGpu {
class Liverpool;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Instance;
class Scheduler;
class ShaderCache;

// Whether new graphics pipelines are created on worker threads, with a draw skipped while its
// pipeline is not ready yet (#521). bb-run --sync-pipelines turns it off.
extern bool async_pipeline_builds;

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    Shader::Info info;
    ModuleList modules{};

    Program() = default;
    Program(Shader::HwStage stage, Shader::SwStage l_stage, Shader::ShaderParams params)
        : info{stage, l_stage, params} {}

    void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec) {
        modules.emplace_back(module, std::move(spec));
    }

    void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                      size_t perm_idx) {
        modules.resize(std::max(modules.size(), perm_idx + 1)); // <-- beware of realloc
        modules[perm_idx] = {module, std::move(spec)};
    }
};

struct DrawIndirectParams {
    u16 vertex_sgpr_offset;
    u32 instance_sgpr_offset;
    bool is_indirect;
};

class PipelineCache {
public:
    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool);
    ~PipelineCache();

    void WarmUp();
    // Saves the driver's cache, writes everything queued and closes the store.
    void Sync();

    // Saves the driver's cache of the one live PipelineCache, if any, and waits for the writes.
    // For the end of a run that skips destructors (bb-run).
    static void FlushActive();

    bool LoadComputePipeline(Serialization::Archive& ar);
    bool LoadGraphicsPipeline(Serialization::Archive& ar);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage);

    const GraphicsPipeline* GetGraphicsPipeline(const DrawIndirectParams params = {});

    const ComputePipeline* GetComputePipeline();

    // The game submitted a frame (sceGnmSubmitDone): the GPU thread may wait again, and the
    // pipelines the workers finished leave the pending list.
    void NewFrame();

    using Result = std::tuple<const Shader::Info*, vk::ShaderModule, u64>;
    Result GetProgram(Shader::HwStage hw_stage, Shader::SwStage sw_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding);

    static std::string GetShaderName(Shader::HwStage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

    // The driver's cache data on disk; empty when none is stored for this device and driver.
    std::vector<u8> LoadDriverCache();

private:
    bool RefreshGraphicsKey();
    bool RefreshGraphicsStages();
    bool RefreshComputeKey();

    vk::ShaderModule CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                   const std::span<const u32>& code, size_t perm_idx,
                                   Shader::Backend::Bindings& binding);
    const Shader::RuntimeInfo& BuildRuntimeInfo(Shader::HwStage stage, Shader::SwStage l_stage);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

    // Saving the driver's cache may run on any thread.
    void SaveDriverCache();
    void SaveDriverCacheIfDue();
    // Logs one new pipeline with the time its new shaders and the pipeline itself took on this
    // thread; for one created on a worker, `create` is only the part before it was handed over.
    void LogBuild(std::string_view kind, u64 hash, std::chrono::steady_clock::time_point start,
                  std::chrono::steady_clock::duration create, bool on_worker = false);

    // A graphics pipeline a worker is creating: what Create reads, kept alive until it is done.
    struct PendingGraphics {
        u64 hash{};
        GraphicsPipeline::SerializationSupport sdata{};
        std::array<vk::ShaderModule, MaxShaderStages> modules{};
        std::future<void> done{};
    };
    void CreateOnWorker(GraphicsPipeline& pipeline, std::unique_ptr<PendingGraphics> pending);
    // Whether the pending pipeline of `key`, if any, is ready, waiting for it within the budget.
    bool WaitForPending(const GraphicsPipelineKey& key);
    void RegisterActive();
    void UnregisterActive();

private:
    const Instance& instance;
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    DescriptorHeap desc_heap;
    vk::UniquePipelineCache pipeline_cache;
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    Shader::Pools pools;
    DrawIndirectParams draw_indirect_params{};
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    Shader::Gcn::FetchShaderData* fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    ComputePipelineKey compute_key{};
    std::atomic<u32> num_new_pipelines{}; // new pipelines added to the cache since the game start
    std::atomic<u32> num_new_shaders{}; // shader modules translated since the game start
    std::atomic<u64> build_ns{}; // time spent translating shaders and building pipelines
    u32 num_preloaded{};                // pipelines loaded from the disk cache at start
    // Shaders translated for the pipeline being looked up, for its build line.
    std::optional<std::chrono::steady_clock::time_point> pending_start{};
    std::chrono::steady_clock::duration pending_translate{};
    std::chrono::steady_clock::duration pending_module{};
    u32 pending_shaders{};
    std::atomic<u64> worker_ns{};         // time the workers spent creating pipelines
    std::atomic<u32> num_skipped_draws{}; // draws skipped while their pipeline was being created
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<PendingGraphics>> pending_graphics;
    PipelineWaitBudget wait_budget;
    std::mutex save_mutex;
    u32 saved_pipelines{}; // num_new_pipelines at the last save of the driver's cache
    std::chrono::steady_clock::time_point last_save{};
    // Last, so its threads are joined before anything they use goes. Empty with
    // async_pipeline_builds off.
    std::optional<PipelineWorkers> workers;
};

} // namespace Vulkan

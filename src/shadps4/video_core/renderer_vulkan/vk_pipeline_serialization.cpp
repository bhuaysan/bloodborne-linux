// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/renderer_vulkan/vk_pipeline_serialization.cpp @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-12
// License texts: third_party/shadps4/LICENSES/

#include <chrono>
#include <elf.h>
#include <link.h>
#include <mutex>

#include "common/serdes.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
static constexpr u32 ShaderBinaryVersion = 11u;
static constexpr u32 ShaderMetaVersion = 7u; // 7: InfoPersistent changed (R8.1)
static constexpr u32 PipelineKeyVersion = 3u;
} // namespace Serialization

namespace Vulkan {

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(Serialization::ShaderBinaryVersion);

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    Shader::StageSpecialization& spec, size_t& perm_idx) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != Serialization::ShaderBinaryVersion) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    spec.Deserialize(ar);
    info.Deserialize(ar);
    return true;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    compute_key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    if (!LoadPipelineStage(meta_ar, 0)) {
        return false;
    }

    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    ASSERT(is_new);

    it.value() =
        std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile, *pipeline_cache,
                                          compute_key, *infos[0], modules[0], sdata, true);

    infos.fill(nullptr);
    modules.fill(nullptr);

    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar) {
    graphics_key.Deserialize(ar);

    GraphicsPipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx)) {
            return false;
        }
    }

    const auto [it, is_new] = graphics_pipelines.try_emplace(graphics_key);
    ASSERT(is_new);

    it.value() = std::make_unique<GraphicsPipeline>(
        instance, scheduler, desc_heap, profile, graphics_key, *pipeline_cache, infos,
        runtime_infos, fetch_shader, modules, sdata, true);

    infos.fill(nullptr);
    modules.fill(nullptr);
    fetch_shader = nullptr;

    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage) {
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    size_t perm_idx{};
    if (!LoadShaderMeta(ar, program->info, spec, perm_idx)) {
        return false;
    }

    std::vector<u32> spv{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx),
                                       spv);
    if (spv.empty()) {
        return false;
    }

    // Permutation hash depends on shader variation index. To prevent collisions, we need insert it
    // at the exact position rather than append

    vk::ShaderModule module{};

    auto [it_pgm, new_program] = program_cache.try_emplace(program->info.pgm_hash);
    if (new_program) {
        module = CompileSPV(spv, instance.GetDevice());
        it_pgm.value() = std::move(program);
    } else {
        const auto& it = std::ranges::find(it_pgm.value()->modules, spec, &Program::Module::spec);
        if (it != it_pgm.value()->modules.end()) {
            // A matching permutation is valid only at its original index. A different index means
            // the store holds entries from more than one cache generation, so this pipeline is
            // left to compile at runtime.
            const auto idx = std::distance(it_pgm.value()->modules.begin(), it);
            if (perm_idx != idx) {
                LOG_WARNING(Render_Vulkan,
                            "Cached permutation {} of {}_{:x} conflicts with index {}, skipping "
                            "preload",
                            perm_idx, program->info.hw_stage, program->info.pgm_hash, idx);
                return false;
            }
            module = it->module;
        } else {
            module = CompileSPV(spv, instance.GetDevice());
        }
    }
    it_pgm.value()->InsertPermut(module, std::move(spec), perm_idx);

    infos[stage] = &it_pgm.value()->info;
    modules[stage] = module;
    if (auto& fetch = it_pgm.value()->modules[perm_idx].spec.fetch_shader_data; !fetch.Empty()) {
        fetch_shader = &fetch;
    }

    return true;
}

namespace {
// GNU build id of the running executable, or empty without one. Any change of the translator or
// the renderer gives a new id, so a cache written by another build is never trusted: spv, meta
// and key files of one build always belong together.
std::vector<u8> ExecutableBuildId() {
    std::vector<u8> id;
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) {
            auto& out = *static_cast<std::vector<u8>*>(data);
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const auto& ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_NOTE) {
                    continue;
                }
                const auto* p = reinterpret_cast<const u8*>(info->dlpi_addr + ph.p_vaddr);
                const auto* end = p + ph.p_memsz;
                while (p + sizeof(ElfW(Nhdr)) <= end) {
                    ElfW(Nhdr) nhdr;
                    std::memcpy(&nhdr, p, sizeof(nhdr));
                    const u8* name = p + sizeof(nhdr);
                    const u8* desc = name + ((nhdr.n_namesz + 3u) & ~3u);
                    const u8* next = desc + ((nhdr.n_descsz + 3u) & ~3u);
                    if (next > end) {
                        break;
                    }
                    if (nhdr.n_type == NT_GNU_BUILD_ID && nhdr.n_namesz == 4 &&
                        std::memcmp(name, "GNU", 4) == 0) {
                        out.assign(desc, desc + nhdr.n_descsz);
                        return 1;
                    }
                    p = next;
                }
            }
            return 1; // only the first object, the executable
        },
        &id);
    return id;
}
} // namespace

u32 CacheSchema() {
    // The versions above, the sizes of the structs that are written as raw bytes, and the build
    // id of the executable.
    u64 hash = 0xcbf29ce484222325ull;
    const auto mix = [&hash](u64 v) { hash = (hash ^ v) * 0x100000001b3ull; };
    for (const u64 v : {u64{Serialization::ShaderBinaryVersion}, u64{Serialization::ShaderMetaVersion},
                        u64{Serialization::PipelineKeyVersion}, u64{sizeof(Shader::InfoPersistent)},
                        u64{sizeof(GraphicsPipelineKey)}, u64{sizeof(Shader::StageSpecialization)},
                        u64{sizeof(Shader::Profile)}}) {
        mix(v);
    }
    static const std::vector<u8> build_id = ExecutableBuildId();
    for (const u8 b : build_id) {
        mix(b);
    }
    return static_cast<u32>(hash ^ (hash >> 32));
}

namespace {
// What a VkPipelineCache's data is valid for: the driver checks its own header, this adds the
// driver version, which the header leaves out.
struct DriverCacheKey {
    u32 vendor_id;
    u32 device_id;
    u32 driver_version;
    std::array<u8, VK_UUID_SIZE> uuid;
    bool operator==(const DriverCacheKey&) const = default;
};
constexpr const char* DriverCacheName = "vk_pipeline_cache";
constexpr auto SaveInterval = std::chrono::seconds(10);

DriverCacheKey GetDriverCacheKey(const Instance& instance) {
    DriverCacheKey key{instance.GetVendorID(), instance.GetDeviceID(), instance.GetDriverVersion(),
                       {}};
    const auto uuid = instance.GetPipelineCacheUUID();
    std::copy(uuid.begin(), uuid.end(), key.uuid.begin());
    return key;
}

std::mutex active_mutex;
PipelineCache* active_cache = nullptr;
} // namespace

std::vector<u8> PipelineCache::LoadDriverCache() {
    auto& db = Storage::DataBase::Instance();
    std::vector<u8> blob;
    db.Load(Storage::BlobType::DriverCache, DriverCacheName, blob);
    if (blob.empty()) {
        return {};
    }
    DriverCacheKey stored{};
    if (blob.size() < sizeof(stored)) {
        LOG_WARNING(Render, "Driver pipeline cache is truncated, rebuilding it");
        return {};
    }
    std::memcpy(&stored, blob.data(), sizeof(stored));
    if (stored != GetDriverCacheKey(instance)) {
        LOG_INFO(Render, "Driver pipeline cache is from another device or driver, rebuilding it");
        return {};
    }
    blob.erase(blob.begin(), blob.begin() + sizeof(stored));
    return blob;
}

void PipelineCache::SaveDriverCache() {
    auto& db = Storage::DataBase::Instance();
    if (!db.IsOpened() || !pipeline_cache) {
        return;
    }
    std::scoped_lock lock{save_mutex};
    saved_pipelines = num_new_pipelines.load();
    LOG_INFO(Render, "Pipeline cache: {} pipelines preloaded, {} built and {} shaders translated "
                     "since start, {} ms spent building them",
             num_preloaded, saved_pipelines, num_new_shaders.load(),
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::duration{build_ns.load()})
                 .count());
    last_save = std::chrono::steady_clock::now();
    auto [result, data] = instance.GetDevice().getPipelineCacheData(*pipeline_cache);
    if (result != vk::Result::eSuccess || data.empty()) {
        return;
    }
    std::vector<u8> blob(sizeof(DriverCacheKey) + data.size());
    const auto key = GetDriverCacheKey(instance);
    std::memcpy(blob.data(), &key, sizeof(key));
    std::memcpy(blob.data() + sizeof(key), data.data(), data.size());
    db.Save(Storage::BlobType::DriverCache, DriverCacheName, std::move(blob));
}

void PipelineCache::SaveDriverCacheIfDue() {
    std::unique_lock lock{save_mutex, std::try_to_lock};
    if (!lock.owns_lock() || num_new_pipelines.load() == saved_pipelines ||
        std::chrono::steady_clock::now() - last_save < SaveInterval) {
        return;
    }
    lock.unlock();
    SaveDriverCache();
}

void PipelineCache::RegisterActive() {
    std::scoped_lock lock{active_mutex};
    active_cache = this;
}

void PipelineCache::UnregisterActive() {
    std::scoped_lock lock{active_mutex};
    if (active_cache == this) {
        active_cache = nullptr;
    }
}

void PipelineCache::FlushActive() {
    std::scoped_lock lock{active_mutex};
    if (active_cache) {
        active_cache->SaveDriverCache();
        Storage::DataBase::Instance().Flush();
    }
}

void PipelineCache::WarmUp() {
    auto& db = Storage::DataBase::Instance();
    if (!db.IsOpened()) {
        return;
    }

    // Check if cache is compatible
    std::vector<u8> profile_data{};
    db.Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    Shader::Profile cached_profile{};
    bool compatible = profile_data.size() == sizeof(Shader::Profile);
    if (compatible) {
        std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
        compatible = cached_profile == profile;
    }
    if (!compatible) {
        if (!profile_data.empty() || db.DroppedBlobs() > 0) {
            LOG_WARNING(Render, "Pipeline cache does not match this system, rebuilding it");
        }
        // Blobs of another profile must not be trusted under the new one.
        db.Clear();
        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        db.Save(Storage::BlobType::ShaderProfile, "profile", std::move(profile_data));
        return;
    }

    u32 num_pipelines{};
    u32 num_total_pipelines{};

    db.ForEachBlob(Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
        ++num_total_pipelines;

        bool result{};
        try {
            Serialization::Archive ar{std::move(data)};
            Serialization::Reader pldata{ar};

            u32 version{};
            pldata.Read(version);
            if (version != Serialization::PipelineKeyVersion) {
                return;
            }

            u32 is_compute{};
            pldata.Read(is_compute);

            result = is_compute ? LoadComputePipeline(ar) : LoadGraphicsPipeline(ar);
        } catch (const std::exception& e) {
            // A blob that passed its checksum but does not parse: rebuilt at run time.
            LOG_WARNING(Render_Vulkan, "Dropped a cached pipeline: {}", e.what());
        }
        if (!result) {
            infos.fill(nullptr);
            modules.fill(nullptr);
            fetch_shader = nullptr;
            return;
        }
        ++num_pipelines;
    });

    num_preloaded = num_pipelines;
    LOG_INFO(Render, "Preloaded {} pipelines", num_pipelines);
    if (num_total_pipelines > num_pipelines || db.DroppedBlobs() > 0) {
        LOG_WARNING(Render,
                    "{} cached pipelines and {} cache files were stale or damaged, rebuilding them",
                    num_total_pipelines - num_pipelines, db.DroppedBlobs());
    }
}

void PipelineCache::Sync() {
    SaveDriverCache();
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        walker_func = RegisterWalkerCode(ar.CurrPtr(), walker_func_size);
        ar.Advance(walker_func_size);
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (!fetch_shader_data.Empty()) {
        spec.Write(sizeof(fetch_shader_data));
        fetch_shader_data.Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader

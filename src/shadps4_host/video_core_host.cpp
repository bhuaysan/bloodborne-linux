// The functions video_core and the ImGui renderer core expect from the rest of shadPS4, for
// bbvideocore (docs/design/renderer.md). Fixed no-ops where the port leaves a feature out, and
// host_missing() where a later R4 issue provides the real thing, so that reaching one is loud.

#include <functional>
#include <string>
#include <vector>

#include "common/log.hpp"
#include "common/types.h"
#include "shadps4_host/failure.hpp"
#include "video_core/cache_storage.h"

using bb::shadps4::host_missing;

// IM_ASSERT of shadPS4's ImGui config (imgui/imgui_config.h).
void assert_fail_debug_msg(const char* msg) {
    bb::common::log_fatal("shadps4", std::string("ImGui assertion failed: ") + msg);
}

// The pipeline cache's storage (cache_storage.cpp, left out with miniz).
// Storage::pipeline_cache_enabled keeps the cache off, so the storage is never opened:
// Close has nothing to do and the rest is unreachable.
namespace Storage {

[[noreturn]] static void no_cache(const char* what) {
    host_missing(std::string("Storage::DataBase::") + what + ": the pipeline cache is off");
}

void DataBase::Open() { no_cache("Open"); }
void DataBase::Close() {}
void DataBase::FinishPreload() { no_cache("FinishPreload"); }
bool DataBase::Save(BlobType, const std::string&, std::vector<u8>&&) { no_cache("Save"); }
bool DataBase::Save(BlobType, const std::string&, std::vector<u32>&&) { no_cache("Save"); }
void DataBase::Load(BlobType, const std::string&, std::vector<u8>&) { no_cache("Load"); }
void DataBase::Load(BlobType, const std::string&, std::vector<u32>&) { no_cache("Load"); }
void DataBase::ForEachBlob(BlobType, const std::function<void(std::vector<u8>&&)>&) {
    no_cache("ForEachBlob");
}

}  // namespace Storage

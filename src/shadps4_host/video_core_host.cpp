// The functions video_core and the ImGui renderer core expect from the rest of shadPS4, for
// bbvideocore (docs/design/renderer.md). Fixed no-ops where the port leaves a feature out, and
// host_missing() where a later R4 issue provides the real thing, so that reaching one is loud.

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "common/log.hpp"
#include "common/stb.h"
#include "common/types.h"
#include "core/memory.h"
#include "sdl_window.h"
#include "shadps4_host/failure.hpp"
#include "video_core/cache_storage.h"
#include "video_core/renderdoc.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

using bb::shadps4::host_missing;

// IM_ASSERT of shadPS4's ImGui config (imgui/imgui_config.h).
void assert_fail_debug_msg(const char* msg) {
    bb::common::log_fatal("shadps4", std::string("ImGui assertion failed: ") + msg);
}

// PNG decoding: see common/stb.h.
stbi_uc* stbi_load_from_memory(const stbi_uc*, int, int*, int*, int*, int) {
    host_missing("stbi_load_from_memory: PNG decoding is not built");
}
void stbi_image_free(void*) { host_missing("stbi_image_free: PNG decoding is not built"); }

// The pipeline cache's storage (cache_storage.cpp, left out with miniz). EmulatorSettings keeps
// the cache off, so the storage is never opened: Close has nothing to do and the rest is
// unreachable.
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

// RenderDoc (renderdoc.cpp, left out) and the screenshot requests that live in the same file.
// Nothing in the port requests a screenshot.
namespace VideoCore {

void LoadRenderDoc() {}
void StartCapture() {}
void EndCapture() {}
void TriggerCapture() {}
void SetOutputDir(const std::filesystem::path&, const std::string&) {}
bool IsRenderDocLoaded() { return false; }
void RequestScreenshot(ScreenshotRequest) { host_missing("VideoCore::RequestScreenshot"); }
u32 ConsumeGameOnlyScreenshotRequests() { return 0; }
u32 ConsumeWithOverlaysScreenshotRequests() { return 0; }
ScreenshotRequests ConsumeScreenshotRequests() { return {}; }

}  // namespace VideoCore

// Guest memory: #430.
namespace Core {

[[noreturn]] static void no_memory(const char* what) {
    host_missing(std::string("Core::") + what + " (#430)");
}

const AddressSpace::BackingPageTable& AddressSpace::BackingPages() const {
    no_memory("AddressSpace::BackingPages");
}
void AddressSpace::Protect(VAddr, u64, MemoryPermission) { no_memory("AddressSpace::Protect"); }
// Used only by the userfaultfd tracker, which EmulatorSettings keeps off.
std::vector<AddressSpace::Region> AddressSpace::GetUsableRegions() {
    no_memory("AddressSpace::GetUsableRegions");
}
AddressSpace& MemoryManager::GetAddressSpace() { no_memory("MemoryManager::GetAddressSpace"); }
bool MemoryManager::IsValidGpuMapping(VAddr, u64) { no_memory("MemoryManager::IsValidGpuMapping"); }
bool MemoryManager::IsValidMapping(VAddr, u64) { no_memory("MemoryManager::IsValidMapping"); }
u64 MemoryManager::ClampRangeSize(VAddr, u64) { no_memory("MemoryManager::ClampRangeSize"); }
void MemoryManager::CopySparseMemory(VAddr, u8*, u64) {
    no_memory("MemoryManager::CopySparseMemory");
}
bool MemoryManager::TryWriteBacking(void*, const void*, u64) {
    no_memory("MemoryManager::TryWriteBacking");
}

}  // namespace Core

// The window: #432. No native window behind it yet, so GetWindowInfo reports a headless one, with
// which the instance creates its device without a surface.
namespace Frontend {

s32 WindowSDL::GetWidth() const { host_missing("Frontend::WindowSDL::GetWidth (#432)"); }
s32 WindowSDL::GetHeight() const { host_missing("Frontend::WindowSDL::GetHeight (#432)"); }
SDL_Window* WindowSDL::GetSDLWindow() const {
    host_missing("Frontend::WindowSDL::GetSDLWindow (#432)");
}
WindowSystemInfo WindowSDL::GetWindowInfo() const { return {}; }

}  // namespace Frontend

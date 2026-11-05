#include "hle/hle.hpp"

#include <sys/mman.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/log.hpp"
#include "common/memory.hpp"
#include "hle/nid_table.hpp"

extern "C" void bb_hle_stub_entry();   // stub_entry.S
extern "C" void bb_hle_trace_entry();  // stub_entry.S

namespace bb::hle {
namespace {

constexpr std::size_t thunk_size = 24;

// Owns the executable pages that hold the thunks: `mov r11, info; jmp entry`. A page is writable only while a thunk is
// written into it; resolve() runs before game code, so no thread executes it meanwhile.
class ThunkPages {
public:
    std::uintptr_t add(const void* info_ptr, void (*entry_fn)()) {
        const std::size_t page = common::page_size();
        if (page_ == nullptr || used_ + thunk_size > page) {
            page_ = static_cast<std::byte*>(
                common::map_anonymous(page, PROT_READ | PROT_WRITE, "hle: mmap"));
            used_ = 0;
        } else {
            protect(page, PROT_READ | PROT_WRITE);
        }
        std::byte* thunk = page_ + used_;
        const auto entry = reinterpret_cast<std::uint64_t>(entry_fn);
        const auto info = reinterpret_cast<std::uint64_t>(info_ptr);
        std::memcpy(thunk, "\x49\xbb", 2);  // mov r11, imm64
        std::memcpy(thunk + 2, &info, 8);
        std::memcpy(thunk + 10, "\xff\x25\x00\x00\x00\x00", 6);  // jmp [rip+0]
        std::memcpy(thunk + 16, &entry, 8);
        used_ += thunk_size;
        protect(page, PROT_READ | PROT_EXEC);
        return reinterpret_cast<std::uintptr_t>(thunk);
    }

private:
    void protect(std::size_t page, int prot) {
        common::protect(page_, page, prot, "hle: ");
    }

    std::byte* page_ = nullptr;
    std::size_t used_ = 0;
};

// One traced implementation (debug builds). `impl` must stay the first member: trace_entry jumps
// through it.
struct TraceInfo {
    void* impl;
    std::string library, nid, channel;
    std::string_view name;
};

struct Registry {
    std::mutex mutex;
    std::unordered_map<std::string, void*> impls;              // import_key(library, nid)
    std::unordered_map<std::string, std::uintptr_t> resolved;  // import_key(library, nid)
    std::deque<StubInfo> stubs;                                // stable addresses
    std::deque<TraceInfo> traces;                              // stable addresses
    ThunkPages thunks;
    std::vector<DataBlock> data_blocks;
    std::atomic<StubObserver*> observer = nullptr;
};

// Leaked on purpose: thunks point into `stubs`, and a thread or atexit handler can
// still call a stub while the process exits, so the registry is never destroyed.
Registry& registry() {
    static Registry& r = *new Registry;
    return r;
}

// The address resolve() hands out for `impl`. In debug builds, when the filter at resolve time
// logs the import's channel at trace level, a thunk that logs each call and jumps on to `impl`;
// otherwise `impl` itself. Called with the registry locked.
std::uintptr_t traced(Registry& r, const loader::Import& imp, void* impl) {
#ifndef NDEBUG
    std::string channel = "hle." + imp.library;
    if (common::log_enabled(common::LogLevel::trace, channel)) {
        const TraceInfo& info = r.traces.emplace_back(impl, imp.library, imp.nid,
                                                      std::move(channel), nid_name(imp.nid));
        return r.thunks.add(&info, &bb_hle_trace_entry);
    }
#else
    (void)r;
    (void)imp;
#endif
    return reinterpret_cast<std::uintptr_t>(impl);
}

}  // namespace

std::string_view nid_name(std::string_view nid) {
    const std::span<const NidName> table = nid_table();
    const auto it = std::ranges::lower_bound(table, nid, {}, &NidName::nid);
    return it != table.end() && it->nid == nid ? it->name : "?";
}

void register_function(std::string_view library, std::string_view nid, void* impl) {
    Registry& r = registry();
    const std::lock_guard lock(r.mutex);
    const std::string k = import_key(library, nid);
    if (r.resolved.contains(k) || !r.impls.emplace(k, impl).second) {
        throw std::logic_error(std::format("hle: {} {} registered twice or after it was resolved",
                                           library, nid));
    }
}

std::uintptr_t resolve(const loader::Import& imp) {
    Registry& r = registry();
    const std::lock_guard lock(r.mutex);
    const std::string k = import_key(imp.library, imp.nid);
    if (auto it = r.resolved.find(k); it != r.resolved.end()) {
        return it->second;
    }
    std::uintptr_t address;
    if (auto it = r.impls.find(k); it != r.impls.end()) {
        address = traced(r, imp, it->second);
    } else if (imp.type == loader::stt_object) {
        auto* block = static_cast<std::byte*>(
            common::map_anonymous(data_block_size, PROT_READ | PROT_WRITE, "hle: mmap"));
        r.data_blocks.push_back({imp.library, imp.nid, block});
        address = reinterpret_cast<std::uintptr_t>(block);
    } else {
        const StubInfo& stub =
            r.stubs.emplace_back(imp.library, imp.module, imp.nid, nid_name(imp.nid));
        address = r.thunks.add(&stub, &bb_hle_stub_entry);
    }
    r.resolved.emplace(k, address);
    return address;
}

std::uintptr_t import_address(std::string_view library, std::string_view nid) {
    Registry& r = registry();
    const std::lock_guard lock(r.mutex);
    const std::string k = import_key(library, nid);
    if (auto it = r.resolved.find(k); it != r.resolved.end()) {
        return it->second;
    }
    common::log_fatal("hle", std::format("import {} {} was never resolved", library, nid));
}

StubObserver* set_stub_observer(StubObserver* observer) {
    return registry().observer.exchange(observer);
}

void on_stub_call(const StubInfo* stub, const std::uint64_t args[6]) {
    const std::uint64_t call = ++stub->calls;
    if (StubObserver* observer = registry().observer) {
        observer->on_call(*stub, args);
    } else if (should_log(call)) {
        // printf formatting keeps the text of the M2 stub log: 0 prints as "0", not "0x0".
        char text[512];
        std::snprintf(text, sizeof text,
                      "stub %s (%s) %s %.*s call %" PRIu64 ": %#" PRIx64 " %#" PRIx64 " %#" PRIx64
                      " %#" PRIx64 " %#" PRIx64 " %#" PRIx64,
                      stub->library.c_str(), stub->module.c_str(), stub->nid.c_str(),
                      static_cast<int>(stub->name.size()), stub->name.data(), call, args[0],
                      args[1], args[2], args[3], args[4], args[5]);
        common::log_message(common::LogLevel::warn, "hle." + stub->library, text);
    }
}

void on_traced_call(const void* info, const std::uint64_t args[6]) {
    const auto* trace = static_cast<const TraceInfo*>(info);
    common::log(common::LogLevel::trace, trace->channel,
                "call {} {} {}: {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}", trace->library, trace->nid,
                trace->name, args[0], args[1], args[2], args[3], args[4], args[5]);
}

Counts counts() {
    Registry& r = registry();
    const std::lock_guard lock(r.mutex);
    return {r.stubs.size(), r.data_blocks.size()};
}

std::vector<DataBlock> data_blocks() {
    Registry& r = registry();
    const std::lock_guard lock(r.mutex);
    return r.data_blocks;
}

}  // namespace bb::hle

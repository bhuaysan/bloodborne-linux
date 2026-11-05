#include "hle/hle.hpp"

#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/log.hpp"
#include "common/memory.hpp"
#include "hle/nid_table.hpp"

extern "C" void bb_hle_stub_entry();   // stub_entry.S
extern "C" void bb_hle_trace_entry();  // stub_entry.S

namespace bb::hle {
namespace {

constexpr std::size_t thunk_size = 24;

// Owns the executable pages that hold the thunks, `mov r11, info; jmp entry`. A page is writable
// only while a thunk is written into it; resolve() runs before game code, so no thread executes
// it meanwhile.
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

struct Resolved {
    std::uintptr_t address;
    Binding binding;
};

struct Registry {
    std::mutex mutex;
    // All keyed by import_key(library, nid).
    std::unordered_map<std::string, void*> impls;
    std::unordered_map<std::string, std::uintptr_t> exports;
    std::unordered_map<std::string, Resolved> resolved;
    std::deque<StubInfo> stubs;                                // stable addresses
    std::deque<TraceInfo> traces;                              // stable addresses
    ThunkPages thunks;
    std::vector<DataBlock> data_blocks;
    std::atomic<StubObserver*> observer = nullptr;
};

// Leaked on purpose: thunks point into `stubs` and `traces`, and a thread or atexit handler can
// still call a stub while the process exits, so the registry is never destroyed.
Registry& registry() {
    static Registry& r = *new Registry;
    return r;
}

// What resolve() hands out for the implementation `impl` of `imp`: a trace thunk if this is a
// build without NDEBUG and the filter shows trace for hle.<library> now, else `impl` itself.
// Called with the registry locked.
std::uintptr_t implementation_address(Registry& r, const loader::Import& imp, void* impl) {
#ifndef NDEBUG
    std::string channel = "hle." + imp.library;
    if (common::log_enabled(common::LogLevel::trace, channel)) {
        const TraceInfo& trace = r.traces.emplace_back(impl, imp.library, imp.module, imp.nid,
                                                       std::move(channel), nid_name(imp.nid));
        return r.thunks.add(&trace, &bb_hle_trace_entry);
    }
#else
    (void)r;
    (void)imp;
#endif
    return reinterpret_cast<std::uintptr_t>(impl);
}

void log_shadowed(std::string_view library, std::string_view nid) {
    common::log(common::LogLevel::warn, std::format("hle.{}", library),
                "an export of {} {} shadows its implementation", library, nid);
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
    if (r.exports.contains(k)) {
        log_shadowed(library, nid);
    }
}

void register_export(std::string_view library, std::string_view nid, std::uintptr_t address) {
    Registry& r = registry();
    const std::lock_guard lock(r.mutex);
    const std::string k = import_key(library, nid);
    if (r.resolved.contains(k) || !r.exports.emplace(k, address).second) {
        throw std::logic_error(std::format(
            "hle: export {} {} registered twice or after the import was resolved", library, nid));
    }
    if (r.impls.contains(k)) {
        log_shadowed(library, nid);
    }
}

std::uintptr_t resolve(const loader::Import& imp) {
    Registry& r = registry();
    const std::lock_guard lock(r.mutex);
    const std::string k = import_key(imp.library, imp.nid);
    if (auto it = r.resolved.find(k); it != r.resolved.end()) {
        return it->second.address;
    }
    Resolved out;
    if (auto it = r.exports.find(k); it != r.exports.end()) {
        out = {it->second, Binding::exported};
    } else if (auto it = r.impls.find(k); it != r.impls.end()) {
        out = {implementation_address(r, imp, it->second), Binding::implemented};
    } else if (imp.type == loader::stt_object) {
        auto* block = static_cast<std::byte*>(
            common::map_anonymous(data_block_size, PROT_READ | PROT_WRITE, "hle: mmap"));
        r.data_blocks.push_back({imp.library, imp.nid, block});
        out = {reinterpret_cast<std::uintptr_t>(block), Binding::data_block};
    } else {
        const StubInfo& stub =
            r.stubs.emplace_back(imp.library, imp.module, imp.nid, nid_name(imp.nid));
        out = {r.thunks.add(&stub, &bb_hle_stub_entry), Binding::stub};
    }
    r.resolved.emplace(k, out);
    return out.address;
}

std::optional<Binding> binding(std::string_view library, std::string_view nid) {
    Registry& r = registry();
    const std::lock_guard lock(r.mutex);
    auto it = r.resolved.find(import_key(library, nid));
    return it == r.resolved.end() ? std::nullopt : std::optional(it->second.binding);
}

std::uintptr_t import_address(std::string_view library, std::string_view nid) {
    Registry& r = registry();
    const std::lock_guard lock(r.mutex);
    const std::string k = import_key(library, nid);
    if (auto it = r.resolved.find(k); it != r.resolved.end()) {
        return it->second.address;
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
        // The message of #44's stub log, kept byte for byte (printf's %#x writes 0 as "0").
        const std::string channel = "hle." + stub->library;
        if (common::log_enabled(common::LogLevel::warn, channel)) {
            std::array<char, 512> message{};
            std::snprintf(message.data(), message.size(),
                          "stub %s (%s) %s %.*s call %" PRIu64 ": %#" PRIx64 " %#" PRIx64
                          " %#" PRIx64 " %#" PRIx64 " %#" PRIx64 " %#" PRIx64,
                          stub->library.c_str(), stub->module.c_str(), stub->nid.c_str(),
                          static_cast<int>(stub->name.size()), stub->name.data(), call, args[0],
                          args[1], args[2], args[3], args[4], args[5]);
            common::log_message(common::LogLevel::warn, channel, message.data());
        }
    }
}

void on_traced_call(const TraceInfo* trace, std::uint64_t caller, const std::uint64_t args[6]) {
    common::log(common::LogLevel::trace, trace->channel,
                "call {} ({}) {} {} from {:#x}: {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}",
                trace->library, trace->module, trace->nid, trace->name, caller, args[0], args[1],
                args[2], args[3], args[4], args[5]);
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

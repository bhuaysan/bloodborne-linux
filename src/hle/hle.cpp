#include "hle/hle.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
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
#include <system_error>
#include <unordered_map>
#include <vector>

#include "hle/nid_table.hpp"

extern "C" void bb_hle_stub_entry();  // stub_entry.S

namespace bb::hle {
namespace {

constexpr std::size_t thunk_size = 24;

void* map(std::size_t size, int prot) {
    void* p = mmap(nullptr, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        throw std::system_error(errno, std::generic_category(), "hle: mmap");
    }
    return p;
}

// Owns the executable pages that hold the thunks. A page is writable only while a thunk is
// written into it; resolve() runs before game code, so no thread executes it meanwhile.
class ThunkPages {
public:
    std::uintptr_t add(const StubInfo* stub) {
        const std::size_t page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        if (page_ == nullptr || used_ + thunk_size > page) {
            page_ = static_cast<std::byte*>(map(page, PROT_READ | PROT_WRITE));
            used_ = 0;
        } else {
            protect(page, PROT_READ | PROT_WRITE);
        }
        std::byte* thunk = page_ + used_;
        const auto entry = reinterpret_cast<std::uint64_t>(&bb_hle_stub_entry);
        const auto info = reinterpret_cast<std::uint64_t>(stub);
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
        if (mprotect(page_, page, prot) != 0) {
            throw std::system_error(errno, std::generic_category(), "hle: mprotect");
        }
    }

    std::byte* page_ = nullptr;
    std::size_t used_ = 0;
};

struct Registry {
    std::mutex mutex;
    std::unordered_map<std::string, void*> impls;              // key(library, nid)
    std::unordered_map<std::string, std::uintptr_t> resolved;  // key(library, nid)
    std::deque<StubInfo> stubs;                                // stable addresses
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

// Names come from C strings, so they contain no '\0'.
std::string key(std::string_view library, std::string_view nid) {
    std::string k(library);
    k += '\0';
    k += nid;
    return k;
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
    const std::string k = key(library, nid);
    if (r.resolved.contains(k) || !r.impls.emplace(k, impl).second) {
        throw std::logic_error(std::format("hle: {} {} registered twice or after it was resolved",
                                           library, nid));
    }
}

std::uintptr_t resolve(const loader::Import& imp) {
    Registry& r = registry();
    const std::lock_guard lock(r.mutex);
    const std::string k = key(imp.library, imp.nid);
    if (auto it = r.resolved.find(k); it != r.resolved.end()) {
        return it->second;
    }
    std::uintptr_t address;
    if (auto it = r.impls.find(k); it != r.impls.end()) {
        address = reinterpret_cast<std::uintptr_t>(it->second);
    } else if (imp.type == loader::stt_object) {
        auto* block = static_cast<std::byte*>(map(data_block_size, PROT_READ | PROT_WRITE));
        r.data_blocks.push_back({imp.library, imp.nid, block});
        address = reinterpret_cast<std::uintptr_t>(block);
    } else {
        const StubInfo& stub =
            r.stubs.emplace_back(imp.library, imp.module, imp.nid, nid_name(imp.nid));
        address = r.thunks.add(&stub);
    }
    r.resolved.emplace(k, address);
    return address;
}

std::uintptr_t import_address(std::string_view library, std::string_view nid) {
    Registry& r = registry();
    const std::lock_guard lock(r.mutex);
    const std::string k = key(library, nid);
    if (auto it = r.resolved.find(k); it != r.resolved.end()) {
        return it->second;
    }
    std::fprintf(stderr, "hle: import %.*s %.*s was never resolved\n",
                 static_cast<int>(library.size()), library.data(), static_cast<int>(nid.size()),
                 nid.data());
    std::abort();
}

void set_stub_observer(StubObserver* observer) {
    registry().observer = observer;
}

void on_stub_call(const StubInfo* stub, const std::uint64_t args[6]) {
    const std::uint64_t call = ++stub->calls;
    if (StubObserver* observer = registry().observer) {
        observer->on_call(*stub, args);
    } else if (should_log(call)) {
        std::fprintf(stderr,
                     "hle: stub %s (%s) %s %.*s call %" PRIu64 ": %#" PRIx64 " %#" PRIx64
                     " %#" PRIx64 " %#" PRIx64 " %#" PRIx64 " %#" PRIx64 "\n",
                     stub->library.c_str(), stub->module.c_str(), stub->nid.c_str(),
                     static_cast<int>(stub->name.size()), stub->name.data(), call, args[0],
                     args[1], args[2], args[3], args[4], args[5]);
    }
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

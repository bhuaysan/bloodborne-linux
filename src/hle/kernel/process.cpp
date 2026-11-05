// libkernel process basics that libc.prx and the eboot call during start-up: errno, the stack
// guard, the process id and name, signals and exit (docs/design/m3-core-os.md, "Process").
// Call sites are link addresses in libc.prx unless the eboot is named.

#include <sys/random.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"

namespace bb::hle::kernel {
namespace {

constexpr std::string_view library = "libkernel";

// A guest sigset_t: 128 bits. libc.prx keeps the old mask of sigprocmask in the 16 bytes below
// its stack guard (0x53f78) and the setjmp mask at +0x48 of a 0x58-byte jmp_buf (0x2fcc2).
constexpr std::size_t guest_sigset_size = 16;

// Until the logger of m3-core-os.md ("Logging", #84) exists: one line on stderr with the caller,
// the return address of the HLE function, a runtime address in the calling module.
template <class... Args>
void log(const void* caller, std::format_string<Args...> format, Args&&... args) {
    const std::string line = std::format("hle.{}: {} (caller {})\n", library,
                                         std::format(format, std::forward<Args>(args)...), caller);
    std::fputs(line.c_str(), stderr);
}

// For calls that libc.prx makes often (sigprocmask around every setjmp and longjmp): logs call 1
// to 10, then every 1000th, as for stubs.
template <class... Args>
void log_often(std::atomic<std::uint64_t>& calls, const void* caller,
               std::format_string<Args...> format, Args&&... args) {
    if (should_log(++calls)) {
        log(caller, format, std::forward<Args>(args)...);
    }
}

#define BB_CALLER() __builtin_return_address(0)

// ---- errno and stack guard -------------------------------------------------------------------

// __error (9BcDykPmo1I): the calling thread's guest errno.
int* error_location() {
    return guest_errno_location();
}

// __stack_chk_guard (f7uOxY9mM1U): the canary that every pinned ELF loads in its prologues and
// compares in its epilogues (function 0x1040: load at 0x1053, compare at 0x107e). Its block is
// filled when the import is resolved, before any game code runs. Never 0, so that a zeroed stack
// slot cannot pass the check.
void fill_stack_guard(std::byte* block) {
    std::uint64_t guard = 0;
    while (guard == 0) {
        if (getrandom(&guard, sizeof guard, 0) != static_cast<ssize_t>(sizeof guard)) {
            std::perror("hle.libkernel: getrandom for the stack guard");
            std::abort();
        }
    }
    std::memcpy(block, &guard, sizeof guard);
}

// __stack_chk_fail (Ou3iL1abvng): a function's canary was overwritten.
[[noreturn]] void stack_check_fail() {
    log(BB_CALLER(), "stack guard overwritten, aborting");
    std::fflush(nullptr);
    std::abort();
}

// ---- process id and name ---------------------------------------------------------------------

std::int32_t get_pid() {
    return static_cast<std::int32_t>(::getpid());
}

// __progname (djxxOmW6-aw): data, a `const char*`. libc's getprogname (0x2ef70) returns the
// pointer in the block's first cell. The string lives in the same block, so the harness snapshot
// covers both.
void fill_progname(std::byte* block) {
    constexpr std::string_view name = "eboot.bin";
    constexpr std::size_t offset = 0x40;
    static_assert(offset + name.size() < data_block_size);
    std::memcpy(block + offset, name.data(), name.size());  // NUL from the zero fill
    const std::byte* text = block + offset;
    std::memcpy(block, &text, sizeof text);
}

// sysctl (DFmMT80xcNI). libc.prx calls it from one function (0x52ee0, sites 0x52f50 and
// 0x52fe1), with the MIB {1, 14, 32, pid}, first for the size, then for the records. It walks
// them as variable-length records (length at +0, type at +4, a start and end address at +8 and
// +0x10, a path at +0x88), looks for the one that holds an address and maps its file: a map of the
// process's memory. We report an empty map: libc then finds no record and returns -1, as for a
// failed sysctl. Any other MIB is logged and refused.
std::int32_t sys_ctl(const std::int32_t* name, std::uint32_t name_length, void* old_data,
                     std::uint64_t* old_length, const void* new_data, std::uint64_t new_length) {
    const void* caller = BB_CALLER();
    if (name == nullptr || old_length == nullptr) {
        log(caller, "sysctl without a MIB or a length pointer: refused");
        return posix_fail(EFAULT);
    }
    if (name_length == 4 && name[0] == 1 && name[1] == 14 && name[2] == 32 &&
        new_data == nullptr) {
        log(caller, "sysctl kern.proc.vmmap pid {} -> empty map", name[3]);
        *old_length = 0;
        return 0;
    }
    std::string mib;
    for (std::uint32_t i = 0; i < name_length && i < 8; ++i) {
        mib += std::format("{}{}", i == 0 ? "" : ".", name[i]);
    }
    log(caller, "sysctl {} (length {}, old {}, new {} size {}) unsupported: refused", mib,
        name_length, old_data, new_data, new_length);
    return posix_fail(ENOENT);
}

// ---- signals ---------------------------------------------------------------------------------
// None of these touch the host's signal state: game code gets no signals delivered (M3).

std::uintptr_t signal_handler(std::int32_t signal, std::uintptr_t handler) {
    // libc.prx tail-calls it as signal(9, 0) at 0x13027.
    log(BB_CALLER(), "signal({}, {:#x}) ignored", signal, handler);
    return 0;  // SIG_DFL as the previous handler
}

std::int32_t signal_mask(std::int32_t how, const void* set, void* old_set) {
    static std::atomic<std::uint64_t> calls{0};
    log_often(calls, BB_CALLER(), "sigprocmask({}, {}, {}) ignored", how, set, old_set);
    if (old_set != nullptr) {
        std::memset(old_set, 0, guest_sigset_size);  // nothing is blocked
    }
    return 0;
}

// 6xVpy0Fdq+I, name unknown: the sigprocmask of libc's setjmp (0x2fcc6: how 1, set null, old set
// at +0x48 of the jmp_buf) and longjmp (0x2fd17: how 3, set from +0x48, old set null). It takes
// the arguments of sigprocmask and behaves like our signal_mask.
std::int32_t jmp_signal_mask(std::int32_t how, const void* set, void* old_set) {
    static std::atomic<std::uint64_t> calls{0};
    log_often(calls, BB_CALLER(), "6xVpy0Fdq+I({}, {}, {}) ignored", how, set, old_set);
    if (old_set != nullptr) {
        std::memset(old_set, 0, guest_sigset_size);
    }
    return 0;
}

std::int32_t signal_fill_set(void* set) {
    if (set == nullptr) {
        return posix_fail(EFAULT);
    }
    std::memset(set, 0xff, guest_sigset_size);
    return 0;
}

std::int32_t signal_return(const void* context) {
    // libc.prx 0x547bf, which reports 0xa002000b through OMDRKKAZ8I4 if it returns.
    log(BB_CALLER(), "sigreturn({}) unsupported", context);
    return 0;
}

// ---- status reports and exit -----------------------------------------------------------------

// OMDRKKAZ8I4, name unknown: libc.prx reports an error code (0xa00200xx, the second argument
// always 0). At least one caller cannot continue: the wrapper at 0x1f560 has no code after the
// call (it falls into padding), so libc treats the report as the end of the process. We log and
// abort, after flushing.
[[noreturn]] void report_fatal_status(std::uint32_t code, std::uint64_t argument) {
    log(BB_CALLER(), "fatal status report {:#x} ({:#x}), aborting", code, argument);
    std::fflush(nullptr);
    std::abort();
}

// zE-wXIZjLoM, name unknown: libc.prx reports a code and continues. Its exit paths call it right
// before _exit (0x5ff2a with 0xa0020003 for status 0 and 0xa0020004 otherwise, 0x5ff8a), so it
// must return.
void report_status(std::uint32_t code, std::uint64_t argument) {
    log(BB_CALLER(), "status report {:#x} ({:#x})", code, argument);
}

// _exit (6Z83sYWFlA8): flush the log and end the process without host atexit handlers or static
// destructors, which must not run under the game's threads.
[[noreturn]] void exit_process(std::int32_t status) {
    log(BB_CALLER(), "_exit({})", status);
    std::fflush(nullptr);  // TODO(#84): flush the logger's file too once it exists.
    ::_exit(status);
}

}  // namespace
}  // namespace bb::hle::kernel

BB_HLE_FUNCTION("libkernel", "9BcDykPmo1I", bb::hle::kernel::error_location);
BB_HLE_DATA("libkernel", "f7uOxY9mM1U", bb::hle::kernel::fill_stack_guard);
BB_HLE_FUNCTION("libkernel", "Ou3iL1abvng", bb::hle::kernel::stack_check_fail);
BB_HLE_FUNCTION("libkernel", "HoLVWNanBBc", bb::hle::kernel::get_pid);
BB_HLE_DATA("libkernel", "djxxOmW6-aw", bb::hle::kernel::fill_progname);
BB_HLE_FUNCTION("libkernel", "DFmMT80xcNI", bb::hle::kernel::sys_ctl);
BB_HLE_FUNCTION("libkernel", "VADc3MNQ3cM", bb::hle::kernel::signal_handler);
BB_HLE_FUNCTION("libkernel", "aPcyptbOiZs", bb::hle::kernel::signal_mask);
BB_HLE_FUNCTION("libkernel", "6xVpy0Fdq+I", bb::hle::kernel::jmp_signal_mask);  // name unknown
BB_HLE_FUNCTION("libkernel", "VkTAsrZDcJ0", bb::hle::kernel::signal_fill_set);
BB_HLE_FUNCTION("libkernel", "mo0bFmWppIw", bb::hle::kernel::signal_return);
BB_HLE_FUNCTION("libkernel", "OMDRKKAZ8I4", bb::hle::kernel::report_fatal_status);  // name unknown
BB_HLE_FUNCTION("libkernel", "zE-wXIZjLoM", bb::hle::kernel::report_status);  // name unknown
BB_HLE_FUNCTION("libkernel", "6Z83sYWFlA8", bb::hle::kernel::exit_process);

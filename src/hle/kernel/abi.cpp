#include "hle/kernel/abi.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <span>

#include "common/log.hpp"

namespace bb::hle::kernel {
namespace {

static_assert(sizeof(GuestTimespec) == 16 && sizeof(GuestTimeval) == 16);
static_assert(offsetof(GuestStat, st_mode) == 8 && offsetof(GuestStat, st_atim) == 24);
static_assert(offsetof(GuestStat, st_mtim) == 40 && offsetof(GuestStat, st_size) == 72);
static_assert(offsetof(GuestStat, st_blksize) == 88 && offsetof(GuestStat, st_birthtim) == 104);
static_assert(sizeof(GuestStat) == 120);
static_assert(offsetof(GuestDirent, reclen) == 4 && offsetof(GuestDirent, name) == 8);
static_assert(sizeof(GuestDirent) == 264 && dirent_length(255) == 264);

// Values that are the same on both sides and therefore pass through unchanged.
static_assert(SEEK_SET == 0 && SEEK_CUR == 1 && SEEK_END == 2 && SEEK_DATA == 3 && SEEK_HOLE == 4);
static_assert(O_RDONLY == 0 && O_WRONLY == 1 && O_RDWR == 2);
static_assert(S_IFMT == 0170000 && S_IFIFO == 0010000 && S_IFCHR == 0020000 &&
              S_IFDIR == 0040000 && S_IFBLK == 0060000 && S_IFREG == 0100000 &&
              S_IFLNK == 0120000 && S_IFSOCK == 0140000);
static_assert(DT_UNKNOWN == 0 && DT_FIFO == 1 && DT_CHR == 2 && DT_DIR == 4 && DT_BLK == 6 &&
              DT_REG == 8 && DT_LNK == 10 && DT_SOCK == 12 && DT_WHT == 14);

struct Pair {
    int guest, host;
};

// Guest errno numbers 1 to guest_elast. Missing numbers (EPROCLIM 67, EBADRPC 72 to
// EPROCUNAVAIL 76, EFTYPE 79, EAUTH 80, ENEEDAUTH 81, EDOOFUS 88, ENOTCAPABLE 93, ECAPMODE 94)
// have no host counterpart.
constexpr Pair errnos[] = {
    {1, EPERM},            {2, ENOENT},           {3, ESRCH},            {4, EINTR},
    {5, EIO},              {6, ENXIO},            {7, E2BIG},            {8, ENOEXEC},
    {9, EBADF},            {10, ECHILD},          {11, EDEADLK},         {12, ENOMEM},
    {13, EACCES},          {14, EFAULT},          {15, ENOTBLK},         {16, EBUSY},
    {17, EEXIST},          {18, EXDEV},           {19, ENODEV},          {20, ENOTDIR},
    {21, EISDIR},          {22, EINVAL},          {23, ENFILE},          {24, EMFILE},
    {25, ENOTTY},          {26, ETXTBSY},         {27, EFBIG},           {28, ENOSPC},
    {29, ESPIPE},          {30, EROFS},           {31, EMLINK},          {32, EPIPE},
    {33, EDOM},            {34, ERANGE},          {35, EAGAIN},          {36, EINPROGRESS},
    {37, EALREADY},        {38, ENOTSOCK},        {39, EDESTADDRREQ},    {40, EMSGSIZE},
    {41, EPROTOTYPE},      {42, ENOPROTOOPT},     {43, EPROTONOSUPPORT}, {44, ESOCKTNOSUPPORT},
    {45, EOPNOTSUPP},      {46, EPFNOSUPPORT},    {47, EAFNOSUPPORT},    {48, EADDRINUSE},
    {49, EADDRNOTAVAIL},   {50, ENETDOWN},        {51, ENETUNREACH},     {52, ENETRESET},
    {53, ECONNABORTED},    {54, ECONNRESET},      {55, ENOBUFS},         {56, EISCONN},
    {57, ENOTCONN},        {58, ESHUTDOWN},       {59, ETOOMANYREFS},    {60, ETIMEDOUT},
    {61, ECONNREFUSED},    {62, ELOOP},           {63, ENAMETOOLONG},    {64, EHOSTDOWN},
    {65, EHOSTUNREACH},    {66, ENOTEMPTY},       {68, EUSERS},          {69, EDQUOT},
    {70, ESTALE},          {71, EREMOTE},         {77, ENOLCK},          {78, ENOSYS},
    {82, EIDRM},           {83, ENOMSG},          {84, EOVERFLOW},       {85, ECANCELED},
    {86, EILSEQ},          {87, ENODATA},         {89, EBADMSG},         {90, EMULTIHOP},
    {91, ENOLINK},         {92, EPROTO},          {95, ENOTRECOVERABLE}, {96, EOWNERDEAD},
};

// Single-bit flags, guest bit to host bits. The open access mode is handled separately.
constexpr Pair open_bits[] = {
    {0x4, O_NONBLOCK},  {0x8, O_APPEND},       {0x40, O_ASYNC},       {0x80, O_SYNC},
    {0x100, O_NOFOLLOW}, {0x200, O_CREAT},     {0x400, O_TRUNC},      {0x800, O_EXCL},
    {0x8000, O_NOCTTY}, {0x10000, O_DIRECT},  {0x20000, O_DIRECTORY}, {0x100000, O_CLOEXEC},
};

constexpr Pair prot_bits[] = {{0x1, PROT_READ}, {0x2, PROT_WRITE}, {0x4, PROT_EXEC}};

constexpr Pair map_bits[] = {
    {0x1, MAP_SHARED},     {0x2, MAP_PRIVATE}, {0x10, MAP_FIXED},      {0x40, MAP_NORESERVE},
    {0x400, MAP_STACK},    {0x1000, MAP_ANONYMOUS}, {0x80000, MAP_32BIT},
};

// MS_SYNC is 0 in the guest, so it isn't a bit there.
constexpr Pair msync_bits[] = {{0x1, MS_ASYNC}, {0x2, MS_INVALIDATE}};

struct Clock {
    std::int32_t guest;
    clockid_t host;
};

// Guest clock ids with a host clock. VIRTUAL 1 and PROF 2 have none; 3 and 6 are unassigned.
constexpr Clock clocks[] = {
    {0, CLOCK_REALTIME},   {4, CLOCK_MONOTONIC},           {5, CLOCK_MONOTONIC},
    {7, CLOCK_MONOTONIC},  {8, CLOCK_MONOTONIC},           {9, CLOCK_REALTIME},
    {10, CLOCK_REALTIME},  {11, CLOCK_MONOTONIC},          {12, CLOCK_MONOTONIC},
    {13, CLOCK_REALTIME},  {14, CLOCK_THREAD_CPUTIME_ID}, {15, CLOCK_PROCESS_CPUTIME_ID},
};

Flags translate(std::span<const Pair> table, int value, bool to_host) {
    Flags out;
    unsigned rest = static_cast<unsigned>(value);
    for (const Pair& p : table) {
        const auto from = static_cast<unsigned>(to_host ? p.guest : p.host);
        if ((rest & from) == from) {
            out.value |= to_host ? p.host : p.guest;
            rest &= ~from;
        }
    }
    out.unknown = static_cast<int>(rest);
    return out;
}

Flags translate_open(int value, bool to_host) {
    Flags out = translate(open_bits, value & ~O_ACCMODE, to_host);
    if ((value & O_ACCMODE) == O_ACCMODE) {
        out.unknown |= O_ACCMODE;
    } else {
        out.value |= value & O_ACCMODE;
    }
    return out;
}

int guest_errno_or_eio(int host) {
    if (const auto guest = guest_errno(host)) {
        return *guest;
    }
    static std::atomic<std::uint64_t> reported[4];
    const auto bit = std::uint64_t{1} << (static_cast<unsigned>(host) % 64);
    const bool tracked = host >= 0 && host < 256;
    if (!tracked || (reported[host / 64].fetch_or(bit) & bit) == 0) {
        common::log(common::LogLevel::warn, "hle.libkernel",
                    "host errno {} has no guest counterpart, using EIO", host);
    }
    return 5;
}

thread_local int errno_cell = 0;

// The x86-64 kernel's __O_LARGEFILE. It always sets it in fcntl(F_GETFL), but glibc defines
// O_LARGEFILE as 0 for 64-bit code, so it has no table entry. Every guest file is large-file
// capable, so the bit carries no information.
constexpr int host_o_largefile = 0x8000;

}  // namespace

std::optional<int> guest_errno(int host_errno) {
    for (const Pair& p : errnos) {
        if (p.host == host_errno) {
            return p.guest;
        }
    }
    return std::nullopt;
}

std::optional<int> host_errno(int guest_errno) {
    for (const Pair& p : errnos) {
        if (p.guest == guest_errno) {
            return p.host;
        }
    }
    return std::nullopt;
}

int* guest_errno_location() {
    return &errno_cell;
}

int posix_fail(int host_errno) {
    errno_cell = guest_errno_or_eio(host_errno);
    return -1;
}

int pthread_error(int host_errno) {
    return host_errno == 0 ? 0 : guest_errno_or_eio(host_errno);
}

std::int32_t sce_fail(int host_errno) {
    return host_errno == 0 ? 0 : sce_error(guest_errno_or_eio(host_errno));
}

Flags host_open_flags(int guest) {
    return translate_open(guest, true);
}

Flags guest_open_flags(int host) {
    return translate_open(host & ~host_o_largefile, false);
}

Flags host_prot(int guest) {
    return translate(prot_bits, guest, true);
}

Flags guest_prot(int host) {
    return translate(prot_bits, host, false);
}

Flags host_map_flags(int guest) {
    return translate(map_bits, guest, true);
}

Flags guest_map_flags(int host) {
    return translate(map_bits, host, false);
}

Flags host_msync_flags(int guest) {
    Flags out = translate(msync_bits, guest, true);
    // FreeBSD refuses MS_ASYNC with MS_INVALIDATE (msync(2), ERRORS), so the pair counts as
    // unknown.
    if ((guest & 0x3) == 0x3) {
        out.unknown |= 0x3;
    }
    if ((out.value & MS_ASYNC) == 0) {
        out.value |= MS_SYNC;
    }
    return out;
}

std::optional<int> host_whence(int guest) {
    if (guest < SEEK_SET || guest > SEEK_HOLE) {
        return std::nullopt;
    }
    return guest;
}

std::optional<clockid_t> host_clock(std::int32_t guest) {
    for (const Clock& c : clocks) {
        if (c.guest == guest) {
            return c.host;
        }
    }
    return std::nullopt;
}

GuestTimespec guest_timespec(const timespec& host) {
    return {host.tv_sec, host.tv_nsec};
}

timespec host_timespec(const GuestTimespec& guest) {
    return {static_cast<time_t>(guest.tv_sec), static_cast<long>(guest.tv_nsec)};
}

GuestTimeval guest_timeval(const timeval& host) {
    return {host.tv_sec, host.tv_usec};
}

GuestStat guest_stat(const struct stat& host) {
    GuestStat g{};
    g.st_dev = static_cast<std::uint32_t>(host.st_dev);
    g.st_ino = static_cast<std::uint32_t>(host.st_ino);
    g.st_mode = static_cast<std::uint16_t>(host.st_mode);
    g.st_nlink = static_cast<std::uint16_t>(std::min<nlink_t>(host.st_nlink, 0xffff));
    g.st_uid = host.st_uid;
    g.st_gid = host.st_gid;
    g.st_rdev = static_cast<std::uint32_t>(host.st_rdev);
    g.st_atim = guest_timespec(host.st_atim);
    g.st_mtim = guest_timespec(host.st_mtim);
    g.st_ctim = guest_timespec(host.st_ctim);
    g.st_size = host.st_size;
    g.st_blocks = host.st_blocks;
    g.st_blksize = static_cast<std::uint32_t>(host.st_blksize);
    return g;
}

std::size_t put_dirent(std::span<std::byte> out, std::uint32_t fileno, std::uint8_t type,
                       std::string_view name) {
    const std::size_t length = dirent_length(name.size());
    if (name.empty() || name.size() > 255 || length > out.size()) {
        return 0;
    }
    GuestDirent d{};
    d.fileno = fileno;
    d.reclen = static_cast<std::uint16_t>(length);
    d.type = type;
    d.namlen = static_cast<std::uint8_t>(name.size());
    std::memcpy(d.name, name.data(), name.size());
    std::memcpy(out.data(), &d, length);  // the guest buffer need not be aligned
    return length;
}

}  // namespace bb::hle::kernel

// Guest file descriptors (docs/design/m3-core-os.md, "Files and paths").
// Guest descriptors are numbers in our own table, so the game never sees, closes or reuses the
// runtime's host descriptors. Other kernel modules (mmap, directories) look descriptors up here.
#pragma once

#include <unistd.h>

#include <memory>
#include <mutex>
#include <vector>

namespace bb::hle::kernel {

// One open host descriptor, closed when the last reference goes. A call that holds a reference
// keeps a valid host descriptor while another thread closes the guest descriptor.
struct OpenFile {
    explicit OpenFile(int fd) : host_fd(fd) {}
    ~OpenFile() { ::close(host_fd); }
    OpenFile(const OpenFile&) = delete;
    const int host_fd;
};

// Guest descriptor numbers to open files. A new descriptor gets the lowest free number, as in
// POSIX. Thread-safe.
class FdTable {
public:
    // Takes ownership of `host_fd` and returns the lowest free guest number for it.
    int insert(int host_fd);

    // The open file behind `guest`, or nullptr if it is not open.
    std::shared_ptr<OpenFile> get(int guest) const;

    // Frees `guest`. The host descriptor closes once no call uses it any more. False if `guest`
    // was not open.
    bool remove(int guest);

private:
    mutable std::mutex lock_;
    std::vector<std::shared_ptr<OpenFile>> slots_;
};

// The process-wide table that the HLE functions use. On first use, guest 0, 1 and 2 hold
// duplicates (dup) of the host's stdin, stdout and stderr; a host stream that is closed is
// replaced by /dev/null.
FdTable& fd_table();

// A private copy of host stream `host` (0, 1 or 2) for the guest table, numbered 3 or more so
// that a guest close never reaches a host descriptor the runtime reuses. A closed stream gives
// /dev/null. Returns -1 if neither works.
int copy_host_stream(int host);

}  // namespace bb::hle::kernel

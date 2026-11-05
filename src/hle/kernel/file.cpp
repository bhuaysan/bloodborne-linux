#include "hle/kernel/file.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>

namespace bb::hle::kernel {

int FdTable::insert(int host_fd) {
    auto file = std::make_shared<OpenFile>(host_fd);
    const std::lock_guard guard(lock_);
    auto slot = std::find(slots_.begin(), slots_.end(), nullptr);
    if (slot == slots_.end()) {
        slot = slots_.insert(slot, nullptr);
    }
    *slot = std::move(file);
    return static_cast<int>(slot - slots_.begin());
}

std::shared_ptr<OpenFile> FdTable::get(int guest) const {
    const std::lock_guard guard(lock_);
    if (guest < 0 || static_cast<std::size_t>(guest) >= slots_.size()) {
        return nullptr;
    }
    return slots_[static_cast<std::size_t>(guest)];
}

bool FdTable::remove(int guest) {
    std::shared_ptr<OpenFile> file;  // released after the lock, so a host close never holds it
    const std::lock_guard guard(lock_);
    if (guest < 0 || static_cast<std::size_t>(guest) >= slots_.size()) {
        return false;
    }
    file = std::move(slots_[static_cast<std::size_t>(guest)]);
    return file != nullptr;
}

int copy_host_stream(int host) {
    const int copy = ::fcntl(host, F_DUPFD_CLOEXEC, 3);
    if (copy >= 0) {
        return copy;
    }
    // A closed host stream becomes /dev/null, so that the numbering holds. A plain open
    // would take the lowest free number, which may be the closed stream itself.
    const int null = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null < 0 || null >= 3) {
        return null;
    }
    const int moved = ::fcntl(null, F_DUPFD_CLOEXEC, 3);
    ::close(null);
    return moved;
}

FdTable& fd_table() {
    static FdTable* const table = [] {
        auto* t = new FdTable;  // never destroyed: guest threads may still run at exit
        for (int host = 0; host < 3; ++host) {
            t->insert(copy_host_stream(host));
        }
        return t;
    }();
    return *table;
}

}  // namespace bb::hle::kernel

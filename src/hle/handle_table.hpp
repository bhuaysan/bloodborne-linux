// Handles that the guest holds for our objects (event queues, semaphores, event flags): numbers
// from 1 up, never reused, each to a shared object. A thread that found an object keeps its own
// reference, so the object outlives its removal from the table.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace bb::hle {

template <class T>
class HandleTable {
public:
    // Adds `object` and returns its new handle.
    std::uint64_t insert(std::shared_ptr<T> object) {
        const std::lock_guard lock(mutex_);
        const std::uint64_t handle = next_handle_++;
        objects_.emplace(handle, std::move(object));
        return handle;
    }

    // The object of `handle`, or nullptr.
    std::shared_ptr<T> find(std::uint64_t handle) const {
        const std::lock_guard lock(mutex_);
        const auto it = objects_.find(handle);
        return it == objects_.end() ? nullptr : it->second;
    }

    // Removes `handle` and returns its object, or nullptr if there is none.
    std::shared_ptr<T> take(std::uint64_t handle) {
        const std::lock_guard lock(mutex_);
        const auto it = objects_.find(handle);
        if (it == objects_.end()) {
            return nullptr;
        }
        std::shared_ptr<T> object = std::move(it->second);
        objects_.erase(it);
        return object;
    }

private:
    mutable std::mutex mutex_;
    std::uint64_t next_handle_ = 1;
    std::unordered_map<std::uint64_t, std::shared_ptr<T>> objects_;
};

}  // namespace bb::hle

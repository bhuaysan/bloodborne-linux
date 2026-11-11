#include "common/clock.hpp"

#include <ctime>

namespace bb::common {
namespace {

std::uint64_t monotonic_nsec() {
    timespec t{};
    ::clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<std::uint64_t>(t.tv_sec) * 1'000'000'000u +
           static_cast<std::uint64_t>(t.tv_nsec);
}

const std::uint64_t start_nsec = monotonic_nsec();

}  // namespace

std::uint64_t process_nsec() {
    return monotonic_nsec() - start_nsec;
}

}  // namespace bb::common

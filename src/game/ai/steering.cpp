// The AI agents' steering at the target frame rate (docs/design/frame-rate.md, "The steering
// ring (#570)"): a wrapper that runs the game's steering on every (frame rate / 30)-th call of each
// agent, as often a game second as at 30 FPS, and hands the agent its last direction in between.
#include "game/ai/steering.hpp"

#include <cstddef>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "game/image.hpp"
#include "hook/hook.hpp"

namespace bb::game {
namespace {

template <class T>
T load(const void* object, std::size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}

template <class T>
void store(void* object, std::size_t offset, T value) {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof value);
}

// The point the game's steering returns from a position and a direction (0x01dbc4d2 to
// 0x01dbc529): position + 3 * direction, w = 3 * w + 1, each a multiply then an add (no fused
// multiply-add: ISO C++ mode keeps -ffp-contract=off).
SteerPoint steer_point(const float position[3], const float direction[4]) {
    const float point[4] = {direction[0] * 3.0f + position[0], direction[1] * 3.0f + position[1],
                            direction[2] * 3.0f + position[2], direction[3] * 3.0f + 1.0f};
    SteerPoint out;
    std::memcpy(&out, point, sizeof out);
    return out;
}

// The agents' counts. The game steps each agent on one thread at a time, but different agents on
// several (the CSChrThreads), so the table has a lock. Agents the game destroys stay in it until it
// holds `max_agents`; then it starts over, which costs each agent at most one extra run of the
// game's steering.
std::mutex counts_lock;
std::unordered_map<const void*, unsigned> counts;
constexpr std::size_t max_agents = 4096;

}  // namespace

SteerPoint agent_steer_every(AgentSteer* original, unsigned calls, unsigned& count, void* agent,
                             const FD4TaskData* task) {
    if (calls <= 1) {
        count = 0;
        return original(agent, task);
    }
    void* const character = load<void*>(agent, 0x230);
    if (character == nullptr) {
        count = 0;  // no ring, no state: a constant; the first call with a character runs
        return original(agent, task);
    }
    const bool runs = count == 0;
    count = (count + 1) % calls;
    if (runs) {
        // calls * the frame's step: 2 * 1/60 is the game's 1/30 bit for bit.
        const FD4TaskData step{task->vtable, task->delta_time * static_cast<float>(calls)};
        return original(agent, &step);
    }
    // The writes the game's steering does on every call with a character (0x01dbbd38, 0x01dbbe10).
    store<std::uint8_t>(agent, 0x28c, 1);
    void* const holder = load<void*>(load<void*>(agent, 0x238), 0x58);
    store<std::uint32_t>(holder, 0x1c, load<std::uint32_t>(character, 0x64));

    float position[3];
    float direction[4];
    std::memcpy(position, static_cast<const std::byte*>(character) + 0x20, sizeof position);
    std::memcpy(direction, static_cast<const std::byte*>(agent) + 0x20, sizeof direction);
    return steer_point(position, direction);
}

// 0x01dbbd00
SteerPoint agent_steer(void* agent, const FD4TaskData* task) {
    auto* const original = reinterpret_cast<AgentSteer*>(hook::original(image(), 0x01dbbd00));
    const unsigned calls = frame_rate() / default_frame_rate;
    if (calls <= 1) {
        return original(agent, task);
    }
    unsigned count = 0;
    {
        const std::lock_guard lock(counts_lock);
        if (const auto it = counts.find(agent); it != counts.end()) {
            count = it->second;
        }
    }
    const SteerPoint point = agent_steer_every(original, calls, count, agent, task);
    {
        const std::lock_guard lock(counts_lock);
        if (counts.size() >= max_agents && !counts.contains(agent)) {
            counts.clear();
        }
        counts[agent] = count;
    }
    return point;
}

void forget_agent_steer_counts() {
    const std::lock_guard lock(counts_lock);
    counts.clear();
}

}  // namespace bb::game

BB_WRAP(0x01dbbd00, bb::game::agent_steer, bb::game::agent_steer_prologue);

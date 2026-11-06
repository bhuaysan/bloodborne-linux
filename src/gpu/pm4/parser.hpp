// The PM4 parser core (docs/design/graphics.md, "Parser core"). A Stream is a resumable cursor
// over one submitted command buffer and the INDIRECT_BUFFERs it calls; Parser::run executes its
// packets against a register mirror until the stream ends, waits or fails. A waiting or failing
// stream keeps its position at that packet, so the next run starts there.
#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <functional>
#include <span>

#include "gpu/pm4/packet.hpp"
#include "gpu/pm4/registers.hpp"

namespace bb::pm4 {

enum class RunResult : std::uint8_t { done, waiting, error };

class Stream {
public:
    // The submitted buffer plus three nested INDIRECT_BUFFERs.
    static constexpr unsigned max_depth = 4;

    explicit Stream(std::span<const std::uint32_t> buffer);

    bool done() const { return depth_ == 0; }
    // The next dword to run, or null when done.
    const std::uint32_t* position() const;

private:
    friend class Parser;

    struct Level {
        const std::uint32_t* pos;
        const std::uint32_t* end;
    };

    void pop_finished();

    std::array<Level, max_depth> levels_{};
    unsigned depth_ = 0;
};

class Parser {
public:
    // Called for every type-3 packet that ran.
    using Trace = std::function<void(const Packet&)>;
    // Whether [address, address + bytes) may be read. INDIRECT_BUFFER targets are checked with
    // it; without one, every address is taken as readable.
    using Readable = std::function<bool(const void* address, std::size_t bytes)>;

    explicit Parser(RegisterMirror& regs) : regs_(regs) {}

    void set_trace(Trace trace) { trace_ = std::move(trace); }
    void set_readable(Readable readable) { readable_ = std::move(readable); }

    RunResult run(Stream& stream);

    // The two dwords of the last CONTEXT_CONTROL (load and shadow enables), recorded only.
    std::array<std::uint32_t, 2> context_control() const { return context_control_; }

    // Opcodes the parser runs or skips on purpose; any other is logged once as unknown.
    static bool known_opcode(std::uint8_t opcode);

private:
    enum class Step : std::uint8_t { next, wait, fail };

    Step execute(Stream& stream, const Packet& packet);
    Step indirect_buffer(Stream& stream, const Packet& packet);
    void set_registers(RegSpace space, const Packet& packet);

    RegisterMirror& regs_;
    Trace trace_;
    Readable readable_;
    std::array<std::uint32_t, 2> context_control_{};
    std::bitset<256> logged_opcodes_;
    std::bitset<4> logged_spaces_;
    bool logged_type0_ = false;
};

}  // namespace bb::pm4

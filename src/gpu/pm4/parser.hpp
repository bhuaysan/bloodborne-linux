// The PM4 parser core (docs/design/graphics.md, "Parser core"). A Stream is a resumable cursor
// over one submitted command buffer and the INDIRECT_BUFFERs it calls; Parser::run executes its
// packets against a register mirror until the stream ends, waits or fails. A waiting or failing
// stream keeps its position at that packet, so the next run starts there.
//
// A stream belongs to one of two engines: the draw engine (DE) runs draw command buffers, the
// constant engine (CE) runs constant command buffers. The CE writes CE RAM and dumps it to
// memory; the two engines synchronise through the CE and DE counters, which the parser keeps
// (docs/design/graphics.md, "Constant engine").
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

// The engine that runs a stream: draw (DCB) or constant (CCB).
enum class Engine : std::uint8_t { draw, constant };

class Stream {
public:
    // The submitted buffer plus three nested INDIRECT_BUFFERs.
    static constexpr unsigned max_depth = 4;

    explicit Stream(std::span<const std::uint32_t> buffer, Engine engine = Engine::draw);

    Engine engine() const { return engine_; }
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
    Engine engine_;
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

    // CE RAM, 48 KiB: the game's CCB writers keep every access below 0xC000 bytes.
    static constexpr std::size_t ce_ram_bytes = 0xC000;
    std::span<const std::uint32_t> ce_ram() const { return ce_ram_; }
    // The counters wrap at 2^32; the waits compare their signed difference.
    std::uint32_t ce_counter() const { return ce_counter_; }
    std::uint32_t de_counter() const { return de_counter_; }

    // Opcodes the parser runs or skips on purpose; any other is logged once as unknown.
    static bool known_opcode(std::uint8_t opcode);

private:
    enum class Step : std::uint8_t { next, wait, fail };

    Step execute(Stream& stream, const Packet& packet);
    // A packet of a constant engine stream. `handled` is false for the packets both engines run.
    Step execute_constant(Stream& stream, const Packet& packet, bool& handled);
    Step dump_const_ram(const Packet& packet);
    Step indirect_buffer(Stream& stream, const Packet& packet);
    void set_registers(RegSpace space, const Packet& packet);

    RegisterMirror& regs_;
    Trace trace_;
    Readable readable_;
    std::array<std::uint32_t, 2> context_control_{};
    std::array<std::uint32_t, ce_ram_bytes / 4> ce_ram_{};
    std::uint32_t ce_counter_ = 0;
    std::uint32_t de_counter_ = 0;
    std::bitset<256> logged_opcodes_;
    std::bitset<256> logged_ce_opcodes_;
    std::bitset<4> logged_spaces_;
    bool logged_type0_ = false;
};

}  // namespace bb::pm4

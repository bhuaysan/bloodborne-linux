// The PM4 parser core (docs/design/graphics.md, "Parser core"). A Stream is a resumable cursor
// over one submitted command buffer and the INDIRECT_BUFFERs it calls; Parser::run executes its
// packets against a register mirror until the stream ends, waits or fails. A waiting or failing
// stream keeps its position at that packet, so the next run starts there.
//
// A stream belongs to one of two engines: the draw engine (DE) runs draw command buffers, the
// constant engine (CE) runs constant command buffers. The CE writes CE RAM and dumps it to
// memory; the two engines synchronise through the CE and DE counters, which the parser keeps
// (docs/design/graphics.md, "Constant engine").
//
// The memory packets (WRITE_DATA, the label writes of EVENT_WRITE_EOP, EVENT_WRITE_EOS and
// RELEASE_MEM) run on the CPU in stream order; WAIT_REG_MEM waits on memory or a register
// (docs/design/graphics.md, "Memory packets").
#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_set>

#include "gpu/pm4/packet.hpp"
#include "gpu/pm4/registers.hpp"

namespace bb::pm4 {

enum class RunResult : std::uint8_t { done, waiting, error };

// The engine that runs a stream: draw (DCB) or constant (CCB).
enum class Engine : std::uint8_t { draw, constant };

// Where the parser hands over to the backend: before a memory packet writes (the backend waits
// for the Vulkan work recorded so far) and before a stream waits on memory (it submits that work).
enum class SyncPoint : std::uint8_t { before_write, before_wait };

// The interrupt source of an end-of-pipe interrupt from the graphics queue (shadPS4's GfxEop).
inline constexpr std::uint32_t gfx_eop_interrupt = 0x40;

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
    // Called at each SyncPoint; without one the parser goes ahead at once.
    using Sync = std::function<void(SyncPoint point)>;
    // Called with the source of each interrupt that an EVENT_WRITE_EOP or RELEASE_MEM asks for,
    // after its memory write.
    using Interrupt = std::function<void(std::uint32_t source)>;

    explicit Parser(RegisterMirror& regs) : regs_(regs) {}

    void set_trace(Trace trace) { trace_ = std::move(trace); }
    void set_readable(Readable readable) { readable_ = std::move(readable); }
    void set_sync(Sync sync) { sync_ = std::move(sync); }
    void set_interrupt(Interrupt interrupt) { interrupt_ = std::move(interrupt); }

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

    // The memory packets (memory_packets.cpp). On `next` the caller moves past the packet.
    Step memory_packet(Engine engine, const Packet& packet);
    Step write_data(Engine engine, const Packet& packet);
    Step wait_reg_mem(const Packet& packet);
    // The label write and interrupt of EVENT_WRITE_EOP, EVENT_WRITE_EOS and RELEASE_MEM.
    Step release(std::uint64_t address, std::uint32_t data_sel, std::uint64_t data,
                 std::uint32_t int_sel);
    // Whether [address, address + bytes) is guest memory (`readable`). Logs if not.
    bool accessible(std::uint64_t address, std::size_t bytes);
    void sync(SyncPoint point) const;
    // Whether an unsupported packet variant `key` is seen for the first time (to log it once).
    bool first_time(std::uint32_t key) { return logged_variants_.insert(key).second; }

    RegisterMirror& regs_;
    Trace trace_;
    Readable readable_;
    Sync sync_;
    Interrupt interrupt_;
    std::unordered_set<std::uint32_t> logged_variants_;
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

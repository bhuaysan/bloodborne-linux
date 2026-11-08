#include "gpu/pm4/parser.hpp"

#include <algorithm>
#include <atomic>

#include "common/log.hpp"

namespace bb::pm4 {

namespace {

constexpr std::string_view channel = "gpu.pm4";

// The opcodes the game writes (docs/formats/gnm.md, "PM4 packets"), except the unnamed 0x8C and
// 0x8D. Those the parser doesn't run yet are skipped without a log line; later items run them.
constexpr auto game_opcodes = std::to_array<std::uint8_t>({
    IT_NOP, IT_SET_BASE, IT_CLEAR_STATE, IT_INDEX_BUFFER_SIZE, IT_DISPATCH_DIRECT,
    IT_DISPATCH_INDIRECT, IT_SET_PREDICATION, IT_DRAW_INDIRECT, IT_DRAW_INDEX_INDIRECT,
    IT_INDEX_BASE, IT_DRAW_INDEX_2, IT_CONTEXT_CONTROL, IT_INDEX_TYPE, IT_DRAW_INDIRECT_MULTI,
    IT_DRAW_INDEX_AUTO, IT_NUM_INSTANCES, IT_INDIRECT_BUFFER_CNST, IT_STRMOUT_BUFFER_UPDATE,
    IT_DRAW_INDEX_OFFSET_2, IT_WRITE_DATA, IT_DRAW_INDEX_INDIRECT_MULTI, IT_MEM_SEMAPHORE,
    IT_WAIT_REG_MEM, IT_INDIRECT_BUFFER, IT_COPY_DATA, IT_PFP_SYNC_ME, IT_EVENT_WRITE,
    IT_EVENT_WRITE_EOP, IT_EVENT_WRITE_EOS, IT_RELEASE_MEM__CI__VI, IT_DMA_DATA__CI__VI,
    IT_ACQUIRE_MEM__CI__VI, IT_REWIND__CI__VI, IT_SET_CONFIG_REG, IT_SET_CONTEXT_REG,
    IT_SET_SH_REG, IT_SET_QUEUE_REG__CI__VI, IT_SET_UCONFIG_REG__CI__VI, IT_WRITE_CONST_RAM,
    IT_DUMP_CONST_RAM, IT_INCREMENT_CE_COUNTER, IT_INCREMENT_DE_COUNTER, IT_WAIT_ON_CE_COUNTER,
    IT_WAIT_ON_DE_COUNTER_DIFF,
});

// The constant engine runs WRITE_CONST_RAM, DUMP_CONST_RAM, INCREMENT_CE_COUNTER,
// WAIT_ON_DE_COUNTER_DIFF and INDIRECT_BUFFER_CONST, and NOP and REWIND like the DE. The DE skips
// the CE's packets and the CE skips the other packets of the game, as the two engines of the
// hardware skip each other's packets. WRITE_DATA occurs in CCBs too; each engine runs those whose
// engine select names it (memory_packets.cpp).

bool body_too_short(const Packet& packet, std::size_t dwords) {
    if (packet.body.size() >= dwords) {
        return false;
    }
    common::log(common::LogLevel::error, channel, "packet {:#010x}: body of {} dwords",
                packet.header, packet.body.size());
    return true;
}

// Whether `bytes` from byte offset `offset` lie in CE RAM. Logs if not.
bool in_ce_ram(std::uint32_t offset, std::size_t bytes) {
    if (offset + bytes <= Parser::ce_ram_bytes) {
        return true;
    }
    common::log(common::LogLevel::error, channel, "CE RAM access at {:#x} of {} bytes", offset,
                bytes);
    return false;
}

// The signed distance from counter b to counter a.
constexpr std::int32_t counter_diff(std::uint32_t a, std::uint32_t b) {
    return static_cast<std::int32_t>(a - b);
}

constexpr std::string_view space_name(RegSpace space) {
    constexpr std::array<std::string_view, 4> names = {"config", "SH", "context", "uconfig"};
    return names[static_cast<unsigned>(space)];
}

}  // namespace

Stream::Stream(std::span<const std::uint32_t> buffer, Engine engine,
               std::uint32_t interrupt_source)
    : engine_(engine), interrupt_source_(interrupt_source) {
    levels_[0] = {buffer.data(), buffer.data() + buffer.size()};
    depth_ = 1;
    pop_finished();
}

const std::uint32_t* Stream::position() const {
    return depth_ == 0 ? nullptr : levels_[depth_ - 1].pos;
}

void Stream::pop_finished() {
    while (depth_ > 0 && levels_[depth_ - 1].pos >= levels_[depth_ - 1].end) {
        --depth_;
    }
}

bool Parser::known_opcode(std::uint8_t opcode) {
    return std::ranges::find(game_opcodes, opcode) != game_opcodes.end();
}

RunResult Parser::run(Stream& stream) {
    interrupt_source_ = stream.interrupt_source();
    while (!stream.done()) {
        Stream::Level& level = stream.levels_[stream.depth_ - 1];
        const std::uint32_t header = *level.pos;
        const std::size_t left = static_cast<std::size_t>(level.end - level.pos);
        const std::uint32_t type = header_type(header);
        if ((type == 0 || type == 3) && body_dwords(header) >= left) {
            common::log(common::LogLevel::error, channel,
                        "packet {:#010x} at {} runs past its buffer", header,
                        static_cast<const void*>(level.pos));
            return RunResult::error;
        }
        switch (type) {
        case 2:
            ++level.pos;
            break;
        case 0:
            if (!logged_type0_) {
                logged_type0_ = true;
                common::log(common::LogLevel::warn, channel, "type-0 packet {:#010x} skipped",
                            header);
            }
            level.pos += 1 + body_dwords(header);
            break;
        case 3: {
            const Packet packet{header, std::span(level.pos + 1, body_dwords(header))};
            switch (execute(stream, packet)) {
            case Step::wait:
                return RunResult::waiting;
            case Step::fail:
                return RunResult::error;
            case Step::next:
                break;
            }
            if (trace_) {
                trace_(packet);
            }
            break;
        }
        default:
            common::log(common::LogLevel::error, channel, "type-1 header {:#010x} at {}", header,
                        static_cast<const void*>(level.pos));
            return RunResult::error;
        }
        stream.pop_finished();
    }
    return RunResult::done;
}

// Runs one type-3 packet. On `next` the stream has moved past it; otherwise it stays there.
Parser::Step Parser::execute(Stream& stream, const Packet& packet) {
    const std::uint32_t* const next = packet.body.data() + packet.body.size();
    const auto too_short = [&](std::size_t dwords) { return body_too_short(packet, dwords); };
    if (stream.engine() == Engine::constant) {
        bool handled = true;
        const Step step = execute_constant(stream, packet, handled);
        if (handled) {
            return step;
        }
    }
    switch (packet.opcode()) {
    case IT_SET_CONFIG_REG:
    case IT_SET_SH_REG:
    case IT_SET_CONTEXT_REG:
    case IT_SET_UCONFIG_REG__CI__VI: {
        if (too_short(1)) {
            return Step::fail;
        }
        const RegSpace space = packet.opcode() == IT_SET_CONFIG_REG    ? RegSpace::config
                               : packet.opcode() == IT_SET_SH_REG      ? RegSpace::sh
                               : packet.opcode() == IT_SET_CONTEXT_REG ? RegSpace::context
                                                                       : RegSpace::uconfig;
        set_registers(space, packet);
        break;
    }
    case IT_INDIRECT_BUFFER:
        return too_short(3) ? Step::fail : indirect_buffer(stream, packet);
    case IT_REWIND__CI__VI: {
        if (too_short(1)) {
            return Step::fail;
        }
        // The CPU sets the valid bit once it has written the space after the packet.
        const std::atomic_ref valid(const_cast<std::uint32_t&>(packet.body[0]));
        if ((valid.load(std::memory_order_acquire) >> 31) == 0) {
            return Step::wait;
        }
        break;
    }
    case IT_CONTEXT_CONTROL:
        if (too_short(2)) {
            return Step::fail;
        }
        context_control_ = {packet.body[0], packet.body[1]};
        break;
    case IT_CLEAR_STATE:
        regs_.clear_context();
        break;
    case IT_WRITE_DATA:
    case IT_COPY_DATA:
    case IT_DMA_DATA__CI__VI:
    case IT_WAIT_REG_MEM:
    case IT_MEM_SEMAPHORE:
    case IT_EVENT_WRITE_EOP:
    case IT_EVENT_WRITE_EOS:
    case IT_RELEASE_MEM__CI__VI: {
        const Step step = memory_packet(stream.engine(), packet);
        if (step != Step::next) {
            return step;
        }
        break;
    }
    case IT_INCREMENT_DE_COUNTER:
        if (stream.engine() == Engine::draw) {
            ++de_counter_;
        }
        break;
    case IT_WAIT_ON_CE_COUNTER:
        // The DE waits until the CE counter is ahead of the DE counter.
        if (stream.engine() == Engine::draw && counter_diff(ce_counter_, de_counter_) <= 0) {
            return Step::wait;
        }
        break;
    default:
        if (!known_opcode(packet.opcode()) && !logged_opcodes_[packet.opcode()]) {
            logged_opcodes_[packet.opcode()] = true;
            common::log(common::LogLevel::warn, channel, "unknown opcode {:#04x} (header {:#010x})",
                        packet.opcode(), packet.header);
        }
        break;
    }
    stream.levels_[stream.depth_ - 1].pos = next;
    return Step::next;
}

// The CE side of a packet. On `next` the stream has moved past it; otherwise it stays there.
Parser::Step Parser::execute_constant(Stream& stream, const Packet& packet, bool& handled) {
    const std::uint32_t* const next = packet.body.data() + packet.body.size();
    switch (packet.opcode()) {
    case IT_NOP:
    case IT_REWIND__CI__VI:
    case IT_WRITE_DATA:
        handled = false;
        return Step::next;
    case IT_WRITE_CONST_RAM: {
        // The byte offset in bits 0-15 (bits 0-1 ignored), then the dwords to write.
        if (body_too_short(packet, 1)) {
            return Step::fail;
        }
        const std::uint32_t offset = packet.body[0] & 0xFFFC;
        const auto data = packet.body.subspan(1);
        if (!in_ce_ram(offset, data.size_bytes())) {
            return Step::fail;
        }
        std::ranges::copy(data, ce_ram_.begin() + offset / 4);
        break;
    }
    case IT_DUMP_CONST_RAM: {
        const Step step = dump_const_ram(packet);
        if (step != Step::next) {
            return step;
        }
        break;
    }
    case IT_INCREMENT_CE_COUNTER:
        // CNTRSEL in bits 0-1: bit 0 selects the CE counter, bit 1 the CS partition counter,
        // which nothing waits on here.
        if (body_too_short(packet, 1)) {
            return Step::fail;
        }
        if ((packet.body[0] & 1) != 0) {
            ++ce_counter_;
        }
        break;
    case IT_WAIT_ON_DE_COUNTER_DIFF: {
        // The CE waits while it is `diff` or more increments ahead of the DE.
        if (body_too_short(packet, 1)) {
            return Step::fail;
        }
        const std::int32_t ahead = counter_diff(ce_counter_, de_counter_);
        if (ahead >= 0 && static_cast<std::uint32_t>(ahead) >= packet.body[0]) {
            return Step::wait;
        }
        break;
    }
    case IT_INDIRECT_BUFFER_CNST:
        // The same body as INDIRECT_BUFFER; the game's writer sets the chain bit like the DCB's.
        return body_too_short(packet, 3) ? Step::fail : indirect_buffer(stream, packet);
    default:
        if (!known_opcode(packet.opcode())) {
            handled = false;  // logged as unknown
            return Step::next;
        }
        if (!logged_ce_opcodes_[packet.opcode()]) {
            logged_ce_opcodes_[packet.opcode()] = true;
            common::log(common::LogLevel::warn, channel,
                        "constant engine skips opcode {:#04x} (header {:#010x})",
                        packet.opcode(), packet.header);
        }
        break;
    }
    stream.levels_[stream.depth_ - 1].pos = next;
    return Step::next;
}

// DUMP_CONST_RAM: the CE RAM byte offset in bits 0-15 (bits 0-1 ignored), the dword count in bits
// 0-14, then the destination address low (bits 2-31) and high. The destination is checked with
// `readable` (the guest mappings); the write waits for the recorded work like the memory packets.
Parser::Step Parser::dump_const_ram(const Packet& packet) {
    if (body_too_short(packet, 4)) {
        return Step::fail;
    }
    const std::uint32_t offset = packet.body[0] & 0xFFFC;
    const std::size_t bytes = std::size_t{packet.body[1] & 0x7FFF} * 4;
    auto* const dest = reinterpret_cast<std::uint32_t*>(std::uint64_t{packet.body[3]} << 32 |
                                                        (packet.body[2] & ~3u));
    if (!in_ce_ram(offset, bytes)) {
        return Step::fail;
    }
    if (bytes != 0 && readable_ && !readable_(dest, bytes)) {
        common::log(common::LogLevel::error, channel, "CE RAM dump to {} of {} bytes unmapped",
                    static_cast<const void*>(dest), bytes);
        return Step::fail;
    }
    if (bytes != 0) {
        sync(SyncPoint::before_write);
    }
    std::copy_n(ce_ram_.begin() + offset / 4, bytes / 4, dest);
    return Step::next;
}

// INDIRECT_BUFFER: base address low (bits 2-31) and high (bits 0-15), then the size in dwords
// (bits 0-19) and the chain bit (20). A chained buffer replaces the current one, as in shadPS4;
// the others nest, and so does every one in a compute ring (shadPS4 chains only below the ring).
Parser::Step Parser::indirect_buffer(Stream& stream, const Packet& packet) {
    const auto* buffer = reinterpret_cast<const std::uint32_t*>(
        std::uint64_t{packet.body[1] & 0xFFFF} << 32 | (packet.body[0] & ~3u));
    const std::uint32_t size = packet.body[2] & 0xFFFFF;
    const bool chain = (packet.body[2] >> 20 & 1) != 0;
    if (size != 0 && readable_ && !readable_(buffer, std::size_t{size} * 4)) {
        common::log(common::LogLevel::error, channel, "indirect buffer {} of {} dwords unmapped",
                    static_cast<const void*>(buffer), size);
        return Step::fail;
    }
    Stream::Level& level = stream.levels_[stream.depth_ - 1];
    if (chain && !(stream.engine() == Engine::compute && stream.depth_ == 1)) {
        level = {buffer, buffer + size};
        return Step::next;
    }
    if (stream.depth_ == Stream::max_depth) {
        common::log(common::LogLevel::error, channel, "indirect buffers nested too deep");
        return Step::fail;
    }
    level.pos = packet.body.data() + packet.body.size();
    stream.levels_[stream.depth_++] = {buffer, buffer + size};
    return Step::next;
}

// SET_*_REG: the register offset in bits 0-15 of the first dword (CI's index bits ignored), then
// the values.
void Parser::set_registers(RegSpace space, const Packet& packet) {
    const std::uint32_t offset = packet.body[0] & 0xFFFF;
    const auto values = packet.body.subspan(1);
    if (regs_.write(space, offset, values) < values.size() &&
        !logged_spaces_[static_cast<unsigned>(space)]) {
        logged_spaces_[static_cast<unsigned>(space)] = true;
        common::log(common::LogLevel::warn, channel,
                    "{} register write at offset {:#x} of {} values runs past the space",
                    space_name(space), offset, values.size());
    }
}

}  // namespace bb::pm4

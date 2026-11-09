// SPDX-License-Identifier: GPL-2.0-or-later
//
// The index state and draw packets of the PM4 parser (docs/design/graphics.md, item 27). Each draw
// packet ends in a DrawCall for the backend, which builds the draw's snapshot from the registers.
// Packet layouts are in docs/formats/gnm.md; shadPS4's pm4_cmds.h was read for them (facts only).
#include "common/log.hpp"
#include "gpu/pm4/parser.hpp"

namespace bb::pm4 {

namespace {

constexpr std::string_view channel = "gpu.pm4";

// VGT_INDEX_TYPE bits 0-1: 0 is 16-bit and 1 is 32-bit indices; 2 (8-bit) is not used by the game.
constexpr std::uint32_t index_type_mask = 3;

// The size of a DRAW_INDIRECT and a DRAW_INDEX_INDIRECT argument block, in bytes.
constexpr std::size_t draw_args_bytes = 16;
constexpr std::size_t index_draw_args_bytes = 20;

bool short_body(const Packet& packet, std::size_t dwords) {
    if (packet.body.size() >= dwords) {
        return false;
    }
    common::log(common::LogLevel::error, channel, "packet {:#010x}: body of {} dwords",
                packet.header, packet.body.size());
    return true;
}

}  // namespace

// DRAW_INDIRECT and DRAW_INDEX_INDIRECT: offset from SET_BASE, base vertex register, start
// instance register, draw initiator.
Parser::Step Parser::indirect_draw(Stream& stream, const Packet& packet, DrawCall& call) {
    if (short_body(packet, 4)) {
        return Step::fail;
    }
    const bool indexed = call.kind == DrawKind::index_indirect;
    call.indirect = stream.indirect_base_ == 0 ? 0 : stream.indirect_base_ + packet.body[0];
    call.base_vertex_reg = packet.body[1] & 0xFFFF;
    call.start_instance_reg = packet.body[2] & 0xFFFF;
    call.initiator = packet.body[3];
    if (call.indirect == 0 ||
        !accessible(call.indirect, indexed ? index_draw_args_bytes : draw_args_bytes)) {
        return Step::fail;
    }
    return Step::next;
}

Parser::Step Parser::draw_packet(Stream& stream, const Packet& packet) {
    const auto& b = packet.body;
    DrawCall call;
    call.instances = stream.num_instances_;
    call.index_bytes = (stream.index_type_ & index_type_mask) == 1 ? 4 : 2;
    switch (packet.opcode()) {
    case IT_INDEX_BASE:
        if (short_body(packet, 2)) {
            return Step::fail;
        }
        stream.index_base_ = b[0] | std::uint64_t{b[1] & 0xFFFF} << 32;
        return Step::next;
    case IT_INDEX_BUFFER_SIZE:
        if (short_body(packet, 1)) {
            return Step::fail;
        }
        stream.index_buffer_size_ = b[0];
        return Step::next;
    case IT_INDEX_TYPE:
        if (short_body(packet, 1)) {
            return Step::fail;
        }
        stream.index_type_ = b[0];
        if ((b[0] & index_type_mask) > 1 && first_time(0x2A00 | (b[0] & index_type_mask))) {
            common::log(common::LogLevel::warn, channel, "INDEX_TYPE {:#x}: not 16 or 32 bit", b[0]);
        }
        return Step::next;
    case IT_NUM_INSTANCES:
        if (short_body(packet, 1)) {
            return Step::fail;
        }
        stream.num_instances_ = b[0];
        return Step::next;
    case IT_DRAW_INDEX_AUTO:
        if (short_body(packet, 2)) {
            return Step::fail;
        }
        call.kind = DrawKind::index_auto;
        call.index_count = b[0];
        call.initiator = b[1];
        break;
    case IT_DRAW_INDEX_2:
        // max size, base low, base high, index count, draw initiator.
        if (short_body(packet, 5)) {
            return Step::fail;
        }
        call.kind = DrawKind::index_2;
        call.max_indices = b[0];
        call.index_address = b[1] | std::uint64_t{b[2] & 0xFFFF} << 32;
        call.index_count = b[3];
        call.initiator = b[4];
        break;
    case IT_DRAW_INDEX_OFFSET_2:
        // max size, first index, index count, draw initiator; the buffer is INDEX_BASE.
        if (short_body(packet, 4)) {
            return Step::fail;
        }
        call.kind = DrawKind::index_offset_2;
        call.max_indices = b[0];
        call.first_index = b[1];
        call.index_count = b[2];
        call.initiator = b[3];
        call.index_address = stream.index_base_;
        break;
    case IT_DRAW_INDEX_INDIRECT:
    case IT_DRAW_INDIRECT: {
        call.kind = packet.opcode() == IT_DRAW_INDIRECT ? DrawKind::indirect
                                                        : DrawKind::index_indirect;
        if (call.kind == DrawKind::index_indirect) {
            call.index_address = stream.index_base_;
            call.max_indices = stream.index_buffer_size_;
        }
        const Step step = indirect_draw(stream, packet, call);
        if (step != Step::next) {
            return step;
        }
        break;
    }
    default:
        return Step::next;
    }
    const bool indexed = call.kind != DrawKind::index_auto && call.kind != DrawKind::indirect;
    if (call.kind == DrawKind::index_offset_2 || call.kind == DrawKind::index_2) {
        if (call.index_address == 0 ||
            !accessible(call.index_address + std::uint64_t{call.first_index} * call.index_bytes,
                        std::size_t{call.index_count} * call.index_bytes)) {
            return Step::fail;
        }
    } else if (indexed && call.index_address == 0) {
        common::log(common::LogLevel::error, channel, "packet {:#010x}: no INDEX_BASE", packet.header);
        return Step::fail;
    }
    // An empty draw draws nothing; an indirect draw's counts are only known to the backend.
    const bool empty = call.indirect == 0 && (call.index_count == 0 || call.instances == 0);
    if (draw_ && !empty) {
        draw_(regs_, call);
    }
    return Step::next;
}

}  // namespace bb::pm4

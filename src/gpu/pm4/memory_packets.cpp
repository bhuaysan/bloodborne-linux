// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/amdgpu/pm4_cmds.h (PM4CmdWriteData, PM4CmdWaitRegMem,
//   PM4CmdEventWriteEop, PM4CmdEventWriteEos, PM4CmdReleaseMem) and
//   src/video_core/amdgpu/liverpool.cpp (their cases in ProcessGraphics and ProcessCompute)
//   @0d5f2739d2544d2a35c73e354cba7a32b7a81bea
// Changed for bloodborne-linux, 2026-11-07: field layouts as shifts and masks on the packet body,
// waits that return to the GPU thread instead of yielding, the guest-memory check, the backend
// sync points, unsupported variants logged once and skipped.
//
// The memory packets of the PM4 parser (docs/design/graphics.md, "Memory packets").
#include <atomic>
#include <chrono>

#include "common/log.hpp"
#include "gpu/pm4/parser.hpp"

namespace bb::pm4 {

namespace {

constexpr std::string_view channel = "gpu.pm4";

// The WRITE_DATA destination selects that mean memory: 1 (memory, sync), 2 (TC L2) and 5
// (memory, async).
constexpr bool write_to_memory(std::uint32_t sel) { return sel == 1 || sel == 2 || sel == 5; }

// A 64-bit address from a low dword (with the low `align_bits` cleared) and a high one.
constexpr std::uint64_t address(std::uint32_t lo, std::uint32_t hi, unsigned align_bits = 2) {
    return std::uint64_t{hi} << 32 | (lo & ~((1u << align_bits) - 1));
}

// The GPU clock of DATA_SEL 3 and 4: nanoseconds of the steady clock, as shadPS4 does for 3.
std::uint64_t gpu_clock() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// Stores 4 or 8 bytes with release order, so a CPU thread polling the label sees what came
// before. An unaligned 8-byte value goes as two dwords, low first.
void store(std::uint64_t at, std::uint64_t value, std::size_t bytes) {
    if (bytes == 8 && at % 8 == 0) {
        std::atomic_ref(*reinterpret_cast<std::uint64_t*>(at))
            .store(value, std::memory_order_release);
        return;
    }
    std::atomic_ref(*reinterpret_cast<std::uint32_t*>(at))
        .store(static_cast<std::uint32_t>(value), std::memory_order_release);
    if (bytes == 8) {
        std::atomic_ref(*reinterpret_cast<std::uint32_t*>(at + 4))
            .store(static_cast<std::uint32_t>(value >> 32), std::memory_order_release);
    }
}

std::uint32_t load32(std::uint64_t at) {
    return std::atomic_ref(*reinterpret_cast<std::uint32_t*>(at)).load(std::memory_order_acquire);
}

bool too_short(const Packet& packet, std::size_t dwords) {
    if (packet.body.size() >= dwords) {
        return false;
    }
    common::log(common::LogLevel::error, channel, "packet {:#010x}: body of {} dwords",
                packet.header, packet.body.size());
    return true;
}

}  // namespace

void Parser::sync(SyncPoint point) const {
    if (sync_) {
        sync_(point);
    }
}

bool Parser::accessible(std::uint64_t at, std::size_t bytes) {
    if (!readable_ || readable_(reinterpret_cast<const void*>(at), bytes)) {
        return true;
    }
    common::log(common::LogLevel::error, channel, "memory packet access at {:#x} of {} bytes",
                at, bytes);
    return false;
}

Parser::Step Parser::memory_packet(Engine engine, const Packet& packet) {
    switch (packet.opcode()) {
    case IT_WRITE_DATA:
        return write_data(engine, packet);
    case IT_WAIT_REG_MEM:
        return wait_reg_mem(packet);
    case IT_EVENT_WRITE_EOP:
        // Event control, address low and high (bits 0-15) with INT_SEL (24-25) and DATA_SEL
        // (29-31), data low and high.
        if (too_short(packet, 5)) {
            return Step::fail;
        }
        return release(address(packet.body[1], packet.body[2] & 0xFFFF), packet.body[2] >> 29,
                       std::uint64_t{packet.body[4]} << 32 | packet.body[3],
                       packet.body[2] >> 24 & 3);
    case IT_EVENT_WRITE_EOS: {
        // Event control, address low and high (bits 0-15) with the command (29-31), data. Command
        // 2 writes the data; command 1 stores GDS, which we don't have.
        if (too_short(packet, 4)) {
            return Step::fail;
        }
        const std::uint32_t command = packet.body[2] >> 29;
        if (command != 2) {
            if (first_time(std::uint32_t{IT_EVENT_WRITE_EOS} << 8 | command)) {
                common::log(common::LogLevel::warn, channel, "EVENT_WRITE_EOS command {} skipped",
                            command);
            }
            return Step::next;
        }
        return release(address(packet.body[1], packet.body[2] & 0xFFFF), 1, packet.body[3], 0);
    }
    default:  // IT_RELEASE_MEM__CI__VI
        // Event control, then DST_SEL (bits 16-17), INT_SEL (24-26) and DATA_SEL (29-31), address
        // low and high, data low and high.
        if (too_short(packet, 6)) {
            return Step::fail;
        }
        return release(address(packet.body[2], packet.body[3]), packet.body[1] >> 29,
                       std::uint64_t{packet.body[5]} << 32 | packet.body[4],
                       packet.body[1] >> 24 & 7);
    }
}

// WRITE_DATA: control (DST_SEL in bits 8-11, one address in bit 16, WR_CONFIRM in 20, ENGINE_SEL
// in 30-31), destination low and high, then the data. Each engine runs the packets whose engine
// select names it: 2 the CE, 0 (ME) and 1 (PFP) the DE.
Parser::Step Parser::write_data(Engine engine, const Packet& packet) {
    if (too_short(packet, 3)) {
        return Step::fail;
    }
    const std::uint32_t control = packet.body[0];
    const std::uint32_t dst_sel = control >> 8 & 0xF;
    const bool one_address = (control >> 16 & 1) != 0;
    const bool constant = (control >> 30) == 2;
    const auto data = packet.body.subspan(3);
    if (constant != (engine == Engine::constant) || !write_to_memory(dst_sel)) {
        if (first_time(std::uint32_t{IT_WRITE_DATA} << 8 | (control >> 24 & 0xC0) | dst_sel)) {
            common::log(common::LogLevel::warn, channel,
                        "WRITE_DATA with control {:#010x} skipped on the {} engine", control,
                        engine == Engine::constant ? "constant" : "draw");
        }
        return Step::next;
    }
    const std::uint64_t dest = address(packet.body[1], packet.body[2]);
    if (data.empty() || !accessible(dest, one_address ? 4 : data.size_bytes())) {
        return data.empty() ? Step::next : Step::fail;
    }
    sync(SyncPoint::before_write);
    for (std::size_t i = 0; i < data.size(); ++i) {
        store(one_address ? dest : dest + 4 * i, data[i], 4);
    }
    return Step::next;
}

// WAIT_REG_MEM: FUNCTION (bits 0-2) and MEM_SPACE (4) of the control, the poll address low (or
// the register's dword offset in bits 0-15) and high, the reference, the mask, the poll interval.
// The stream waits until (value & mask) compares true against the reference.
Parser::Step Parser::wait_reg_mem(const Packet& packet) {
    if (too_short(packet, 6)) {
        return Step::fail;
    }
    const std::uint32_t function = packet.body[0] & 7;
    const bool memory = (packet.body[0] >> 4 & 1) != 0;
    std::uint32_t value = 0;
    if (memory) {
        const std::uint64_t at = address(packet.body[1], packet.body[2]);
        if (!accessible(at, 4)) {
            return Step::fail;
        }
        value = load32(at);
    } else {
        value = regs_.get(packet.body[1] & 0xFFFF);
    }
    value &= packet.body[4];
    const std::uint32_t ref = packet.body[3];
    bool pass = false;
    switch (function) {
    case 0: pass = true; break;
    case 1: pass = value < ref; break;
    case 2: pass = value <= ref; break;
    case 3: pass = value == ref; break;
    case 4: pass = value != ref; break;
    case 5: pass = value >= ref; break;
    case 6: pass = value > ref; break;
    default:
        common::log(common::LogLevel::error, channel, "WAIT_REG_MEM with reserved function 7");
        return Step::fail;
    }
    if (!pass) {
        sync(SyncPoint::before_wait);
        return Step::wait;
    }
    return Step::next;
}

// DATA_SEL: 0 nothing, 1 the low data dword, 2 the data qword, 3 and 4 the GPU clock; 5 (GDS) is
// logged once and skipped. INT_SEL: non-zero raises the interrupt, after the write.
Parser::Step Parser::release(std::uint64_t at, std::uint32_t data_sel, std::uint64_t data,
                             std::uint32_t int_sel) {
    if (data_sel > 4) {
        if (first_time(std::uint32_t{IT_RELEASE_MEM__CI__VI} << 8 | data_sel)) {
            common::log(common::LogLevel::warn, channel, "label write with DATA_SEL {} skipped",
                        data_sel);
        }
        data_sel = 0;
    }
    const std::size_t bytes = data_sel == 1 ? 4 : 8;
    if (data_sel != 0 && !accessible(at, bytes)) {
        return Step::fail;
    }
    if (data_sel != 0 || int_sel != 0) {
        sync(SyncPoint::before_write);
    }
    if (data_sel != 0) {
        store(at, data_sel >= 3 ? gpu_clock() : data, bytes);
    }
    if (int_sel != 0 && interrupt_) {
        interrupt_(gfx_eop_interrupt);
    }
    return Step::next;
}

}  // namespace bb::pm4

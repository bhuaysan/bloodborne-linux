// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/amdgpu/pm4_cmds.h (PM4CmdWriteData, PM4CmdCopyData, PM4DmaData,
//   PM4CmdWaitRegMem, PM4CmdMemSemaphore, PM4CmdEventWriteEop, PM4CmdEventWriteEos,
//   PM4CmdReleaseMem) and src/video_core/amdgpu/liverpool.cpp (their cases in ProcessGraphics
//   and ProcessCompute) @0d5f2739d2544d2a35c73e354cba7a32b7a81bea
// Changed for bloodborne-linux, 2026-11-07: field layouts as shifts and masks on the packet body,
// waits that return to the GPU thread instead of yielding, the guest-memory check, the backend
// sync points, COPY_DATA run instead of logged, unsupported variants logged once and skipped.
//
// The memory packets of the PM4 parser (docs/design/graphics.md, "Memory packets").
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstring>

#include "common/log.hpp"
#include "gpu/pm4/parser.hpp"

namespace bb::pm4 {

namespace {

constexpr std::string_view channel = "gpu.pm4";

// Destination and source selects that mean memory: WRITE_DATA and COPY_DATA use 1 (memory, sync),
// 2 (TC L2) and 5 (memory, async); DMA_DATA uses 0 (memory) and 3 (memory through L2).
constexpr bool write_to_memory(std::uint32_t sel) { return sel == 1 || sel == 2 || sel == 5; }
constexpr bool dma_memory(std::uint32_t sel) { return sel == 0 || sel == 3; }

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

void Parser::write_register(std::uint32_t reg, std::uint32_t value) {
    for (const RegSpace space : {RegSpace::config, RegSpace::sh, RegSpace::context,
                                 RegSpace::uconfig}) {
        const SpaceRange range = space_range(space);
        if (reg >= range.base && reg - range.base < range.size) {
            regs_.write(space, reg - range.base, std::span(&value, 1));
            return;
        }
    }
    if (first_time(0x10000 | reg)) {
        common::log(common::LogLevel::warn, channel, "register write to {:#x} dropped", reg);
    }
}

Parser::Step Parser::memory_packet(Engine engine, const Packet& packet) {
    switch (packet.opcode()) {
    case IT_WRITE_DATA:
        return write_data(engine, packet);
    case IT_COPY_DATA:
        return copy_data(packet);
    case IT_DMA_DATA__CI__VI:
        return dma_data(packet);
    case IT_WAIT_REG_MEM:
        return wait_reg_mem(packet);
    case IT_MEM_SEMAPHORE:
        return mem_semaphore(packet);
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
    if (constant != (engine == Engine::constant) || (dst_sel != 0 && !write_to_memory(dst_sel))) {
        if (first_time(std::uint32_t{IT_WRITE_DATA} << 8 | (control >> 24 & 0xC0) | dst_sel)) {
            common::log(common::LogLevel::warn, channel,
                        "WRITE_DATA with control {:#010x} skipped on the {} engine", control,
                        engine == Engine::constant  ? "constant"
                        : engine == Engine::compute ? "compute"
                                                    : "draw");
        }
        return Step::next;
    }
    if (dst_sel == 0) {  // registers from the dword offset in bits 0-15
        std::uint32_t reg = packet.body[1] & 0xFFFF;
        for (const std::uint32_t value : data) {
            write_register(one_address ? reg : reg++, value);
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

// COPY_DATA: control (SRC_SEL in bits 0-3, DST_SEL in 8-11, COUNT_SEL in 16 for 64 bits), source
// low and high, destination low and high. Sources: 0 register, 1 and 2 memory, 5 the immediate
// source dwords, 9 the GPU clock; destinations: 0 register, 1, 2 and 5 memory. The game copies
// memory to VGT_STRMOUT_DRAW_OPAQUE_BUFFER_FILLED_SIZE (0x107be30).
Parser::Step Parser::copy_data(const Packet& packet) {
    if (too_short(packet, 5)) {
        return Step::fail;
    }
    const std::uint32_t control = packet.body[0];
    const std::uint32_t src_sel = control & 0xF;
    const std::uint32_t dst_sel = control >> 8 & 0xF;
    const std::size_t bytes = (control >> 16 & 1) != 0 ? 8 : 4;
    const bool src_memory = src_sel == 1 || src_sel == 2;
    const bool known_src = src_sel == 0 || src_memory || src_sel == 5 || src_sel == 9;
    if (!known_src || (dst_sel != 0 && !write_to_memory(dst_sel))) {
        if (first_time(std::uint32_t{IT_COPY_DATA} << 8 | dst_sel << 4 | src_sel)) {
            common::log(common::LogLevel::warn, channel, "COPY_DATA with control {:#010x} skipped",
                        control);
        }
        return Step::next;
    }
    std::uint64_t value = 0;
    if (src_sel == 0) {
        const std::uint32_t reg = packet.body[1] & 0xFFFF;
        value = regs_.get(reg) | (bytes == 8 ? std::uint64_t{regs_.get(reg + 1)} << 32 : 0);
    } else if (src_memory) {
        const std::uint64_t src = address(packet.body[1], packet.body[2]);
        if (!accessible(src, bytes)) {
            return Step::fail;
        }
        value = load32(src) | (bytes == 8 ? std::uint64_t{load32(src + 4)} << 32 : 0);
    } else if (src_sel == 5) {
        value = std::uint64_t{packet.body[2]} << 32 | packet.body[1];
    } else {
        value = gpu_clock();
    }
    if (bytes == 4) {
        value &= 0xFFFFFFFF;
    }
    if (dst_sel == 0) {
        const std::uint32_t reg = packet.body[3] & 0xFFFF;
        write_register(reg, static_cast<std::uint32_t>(value));
        if (bytes == 8) {
            write_register(reg + 1, static_cast<std::uint32_t>(value >> 32));
        }
        return Step::next;
    }
    const std::uint64_t dest = address(packet.body[3], packet.body[4]);
    if (!accessible(dest, bytes)) {
        return Step::fail;
    }
    sync(SyncPoint::before_write);
    store(dest, value, bytes);
    return Step::next;
}

// DMA_DATA: control (DST_SEL in bits 20-21, SRC_SEL in 29-30), source low (or the fill dword) and
// high, destination low and high, then the command (byte count in bits 0-20, SAS 26, DAS 27,
// SAIC 28, DAIC 29). Memory to memory copies, a data source fills dwords. GDS, register
// addressing (SAS, DAS) and fixed addresses (SAIC, DAIC) are logged once and skipped.
Parser::Step Parser::dma_data(const Packet& packet) {
    if (too_short(packet, 6)) {
        return Step::fail;
    }
    const std::uint32_t control = packet.body[0];
    const std::uint32_t dst_sel = control >> 20 & 3;
    const std::uint32_t src_sel = control >> 29 & 3;
    const std::uint32_t command = packet.body[5];
    const std::size_t bytes = command & 0x1FFFFF;
    const bool fill = src_sel == 2;
    if (!dma_memory(dst_sel) || !(fill || dma_memory(src_sel)) || (command >> 26 & 0xF) != 0 ||
        (fill && bytes % 4 != 0)) {
        if (first_time(std::uint32_t{IT_DMA_DATA__CI__VI} << 8 | dst_sel << 2 | src_sel)) {
            common::log(common::LogLevel::warn, channel,
                        "DMA_DATA with control {:#010x}, command {:#010x} skipped", control,
                        command);
        }
        return Step::next;
    }
    const std::uint64_t dest = address(packet.body[3], packet.body[4], 0);
    const std::uint64_t src = address(packet.body[1], packet.body[2], 0);
    if (bytes == 0) {
        return Step::next;
    }
    if (!accessible(dest, bytes) || (!fill && !accessible(src, bytes))) {
        return Step::fail;
    }
    sync(SyncPoint::before_write);
    if (fill) {
        auto* const out = reinterpret_cast<std::uint32_t*>(dest);
        std::fill_n(out, bytes / 4, packet.body[1]);
    } else {
        std::memmove(reinterpret_cast<void*>(dest), reinterpret_cast<const void*>(src), bytes);
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

// MEM_SEMAPHORE: the address low (bits 3-31), then the address high, SIGNAL_TYPE (20) and SEM_SEL
// (29-31). shadPS4 takes bits 0-7 of the high part (40-bit addresses); we take bits 0-15 like the
// other packets, which agrees for every guest address (below 2^40) and lets host tests use stack
// memory. The semaphore is a uint64. Select 6 signals: increment, or write 1 with
// SIGNAL_TYPE set. Select 7 waits until it is non-zero and decrements it.
Parser::Step Parser::mem_semaphore(const Packet& packet) {
    if (too_short(packet, 2)) {
        return Step::fail;
    }
    const std::uint64_t at = address(packet.body[0], packet.body[1] & 0xFFFF, 3);
    const std::uint32_t select = packet.body[1] >> 29;
    if (select != 6 && select != 7) {
        if (first_time(std::uint32_t{IT_MEM_SEMAPHORE} << 8 | select)) {
            common::log(common::LogLevel::warn, channel, "MEM_SEMAPHORE select {} skipped",
                        select);
        }
        return Step::next;
    }
    if (!accessible(at, 8)) {
        return Step::fail;
    }
    const std::atomic_ref semaphore(*reinterpret_cast<std::uint64_t*>(at));
    if (select == 6) {
        sync(SyncPoint::before_write);
        if ((packet.body[1] >> 20 & 1) != 0) {
            semaphore.store(1, std::memory_order_release);
        } else {
            semaphore.fetch_add(1, std::memory_order_acq_rel);
        }
        return Step::next;
    }
    std::uint64_t count = semaphore.load(std::memory_order_acquire);
    while (count != 0) {
        if (semaphore.compare_exchange_weak(count, count - 1, std::memory_order_acq_rel)) {
            return Step::next;
        }
    }
    sync(SyncPoint::before_wait);
    return Step::wait;
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
        interrupt_(interrupt_source_);
    }
    return Step::next;
}

}  // namespace bb::pm4

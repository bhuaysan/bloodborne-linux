// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/common/decoder.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include <Zydis/Zydis.h>
#include "common/singleton.h"
#include "common/types.h"

namespace Common {

class DecoderImpl {
public:
    DecoderImpl();
    ~DecoderImpl();

    std::string disassembleInst(ZydisDecodedInstruction& inst, ZydisDecodedOperand* operands,
                                u64 address);
    ZyanStatus decodeInstruction(ZydisDecodedInstruction& inst, ZydisDecodedOperand* operands,
                                 void* data, u64 size = 15);

private:
    ZydisDecoder m_decoder;
    ZydisFormatter m_formatter;
};

using Decoder = Common::Singleton<DecoderImpl>;

} // namespace Common

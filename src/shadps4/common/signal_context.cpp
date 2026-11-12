// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/common/signal_context.cpp @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-12
// License texts: third_party/shadps4/LICENSES/

#include <sys/ucontext.h>
#include "common/signal_context.h"

namespace Common {

void* GetRip(void* ctx) {
    return (void*)((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RIP];
}

bool IsWriteError(void* ctx) {
    return ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_ERR] & 0x2;
}

bool IsExecuteError(void* ctx) {
    return ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_ERR] & 0x10;
}

} // namespace Common

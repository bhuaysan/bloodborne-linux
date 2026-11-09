// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/common/arch.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#pragma once

#if defined(__x86_64__) || defined(_M_X64)
#define ARCH_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define ARCH_ARM64 1
#endif

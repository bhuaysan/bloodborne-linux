// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/common/number_utils.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include "common/types.h"

namespace NumberUtils {

float Uf11ToF32(u16 val);
float Uf10ToF32(u16 val);
float Uf16ToF32(u16 val);
float U2ToUnorm(u8 val);
float S2ToSnorm(s8 val);
float U4ToUnorm(u8 val);
float S4ToSnorm(s8 val);
float U5ToUnorm(u8 val);
float S5ToSnorm(s8 val);
float U6ToUnorm(u8 val);
float S6ToSnorm(s8 val);
float U8ToUnorm(u8 val);
float S8ToSnorm(s8 val);
float U10ToUnorm(u16 val);
float S10ToSnorm(s16 val);
float U16ToUnorm(u16 val);
float S16ToSnorm(s16 val);

} // namespace NumberUtils

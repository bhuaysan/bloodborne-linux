//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/common/string_literal.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include <algorithm>

template <size_t N, typename C = char>
struct StringLiteral {
    static constexpr size_t len = N;

    constexpr StringLiteral(const C (&str)[N]) {
        std::copy_n(str, N, value);
    }

    C value[N]{};
};

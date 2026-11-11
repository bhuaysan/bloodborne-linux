// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/core/libraries/videodec/video_utils.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include "common/types.h"

struct AVFrame;

namespace Libraries::Videodec {

void CopyNV12Data(u8* dst, u64 max_size, const AVFrame& src);

}

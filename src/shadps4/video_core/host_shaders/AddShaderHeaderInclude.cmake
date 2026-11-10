# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
# Source: shadPS4 src/video_core/host_shaders/AddShaderHeaderInclude.cmake @11557f2eb60b3b00712c8887c30852de7615d6df
# Changed for bloodborne-linux, 2026-11-09
# License texts: third_party/shadps4/LICENSES/

file(READ "${HEADER_FILE}" HEADER_CONTENT)
string(REPLACE "#pragma once" "#pragma once\n#include <cstdint>" HEADER_CONTENT "${HEADER_CONTENT}")
file(WRITE "${HEADER_FILE}" "${HEADER_CONTENT}")

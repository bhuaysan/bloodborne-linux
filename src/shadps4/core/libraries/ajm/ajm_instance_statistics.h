// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/core/libraries/ajm/ajm_instance_statistics.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include "core/libraries/ajm/ajm_batch.h"

namespace Libraries::Ajm {

class AjmInstanceStatistics {
public:
    void ExecuteJob(AjmJob& job);
    void Reset();

    static AjmInstanceStatistics& Getinstance();
};

} // namespace Libraries::Ajm

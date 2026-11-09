// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/common/singleton.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include <memory>

namespace Common {

template <class T>
class Singleton {
public:
    static T* Instance() {
        if (!m_instance) {
            m_instance = std::make_unique<T>();
        }
        return m_instance.get();
    }

protected:
    Singleton();
    ~Singleton();

private:
    static inline std::unique_ptr<T> m_instance{};
};

} // namespace Common

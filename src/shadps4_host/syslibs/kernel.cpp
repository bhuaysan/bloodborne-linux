// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/core/libraries/kernel/threads.h (Thread::Run, Join, Stop) and
// src/core/libraries/kernel/kernel.cpp (sceKernelGetSystemSwVersion)
// @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-11: Thread starts and joins guest threads through our
// kernel with upstream's default stack, and logs failures; sceKernelGetSystemSwVersion reports
// the version its header names, without logging. The rest of the file is our own.
// License texts: third_party/shadps4/LICENSES/

// The kernel pieces the system libraries use (docs/design/os-libraries.md): guest threads, the
// process time, the SDK and system versions, and the user on pad slot 1. Each forwards to our
// kernel or user service, or returns the fixed value its header names.

#include <cerrno>
#include <cstdio>
#include <format>

#include "common/log.hpp"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/kernel/threads.h"
#include "core/libraries/kernel/time.h"
#include "core/user_settings.h"
#include "hle/kernel/thread.hpp"
#include "hle/kernel/time.hpp"
#include "hle/system/system.hpp"

namespace Libraries::Kernel {

namespace {
// Upstream's default thread stack (ThrStackDefault). AvPlayer decodes video on these threads.
constexpr std::size_t worker_stack_size = 0x100000;
}  // namespace

void Thread::Run(std::function<void(std::stop_token)>&& f) {
    func = std::move(f);
    bb::hle::kernel::ThreadAttributes attributes;
    attributes.stack_size = worker_stack_size;
    const int error =
        bb::hle::kernel::create_thread_with(&thread, attributes, &Entry, this, nullptr);
    if (error != 0) {
        bb::common::log_fatal("shadps4",
                              std::format("system library thread: create failed, errno {}", error));
    }
}

void* Thread::Entry(void* self) {
    auto* t = static_cast<Thread*>(self);
    t->func(t->stop.get_token());
    return nullptr;
}

void Thread::Join() {
    if (thread == nullptr) {
        return;
    }
    // AvPlayer's workers end by joining themselves (upstream's pthread_join returns EDEADLK and
    // leaves the thread unjoined). Detach instead, so the thread frees itself when it ends. When
    // Stop() joins it at the same time, the detach finds it joining (EINVAL) and the join frees it.
    const bool self = thread == bb::hle::kernel::current_thread();
    const int error =
        self ? bb::hle::kernel::detach_thread(thread) : bb::hle::kernel::join_thread(thread, nullptr);
    if (error != 0 && !(self && error == EINVAL)) {
        bb::common::log(bb::common::LogLevel::error, "shadps4",
                        "system library thread: {} failed, errno {}", self ? "detach" : "join",
                        error);
    }
    thread = nullptr;
}

void Thread::Stop() {
    if (Joinable()) {
        stop.request_stop();
        Join();
    }
    func = nullptr;
    stop = std::stop_source{};
}

u64 sceKernelGetProcessTime() {
    return bb::hle::kernel::process_time_usec();
}

s32 sceKernelGetCompiledSdkVersion(s32* ver) {
    if (ver == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    *ver = compiled_sdk_version;
    return 0;
}

s32 sceKernelGetSystemSwVersion(SwVersionStruct* ret) {
    if (ret == nullptr) {
        return 0;
    }
    const u32 v = system_sw_version;
    ret->hex_representation = v;
    std::snprintf(ret->text_representation, sizeof(ret->text_representation), "%2x.%03x.%03x",
                  v >> 24, (v >> 12) & 0xfff, v & 0xfff);
    return 0;
}

}  // namespace Libraries::Kernel

UserManager::UserManager() {
    user.user_id = bb::hle::system::user_id;
    user.player_index = 1;
    user.logged_in = true;
}

User* UserManager::GetUserByID(s32 user_id) {
    return user_id == user.user_id ? &user : nullptr;
}

User* UserManager::GetUserByPlayerIndex(s32 index) {
    return index == user.player_index ? &user : nullptr;
}

void UserManager::LoginUser(User* u, s32 /*player_index*/) {
    if (u != nullptr) {
        u->logged_in = true;
    }
}

UserManager& HostUserManager() {
    static UserManager& manager = *new UserManager;  // leaked: game threads may call in at exit
    return manager;
}

// The thread helper of shadPS4's libkernel that AudioOut and AvPlayer run their workers on. It
// starts a guest thread through our kernel (src/hle/kernel/thread.cpp), so the worker gets the
// guest TLS and the signal stack like the game's own threads: AvPlayer's file streamer calls the
// game's callbacks from it.
#pragma once

#include <functional>
#include <stop_token>

// Upstream's threads.h brings these through its pthread headers, and audioout.h and
// sdl_audio_out.cpp rely on that.
#include "core/libraries/kernel/time.h"
#include "core/tls.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace bb::hle::kernel {
struct Thread;
}

namespace Libraries::Kernel {

class Thread {
public:
    Thread() = default;
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    ~Thread() { Stop(); }

    // Starts `func` on a new joinable guest thread with upstream's default stack (1 MiB). It gets
    // a stop token that Stop() requests. Aborts if the thread can't be created.
    void Run(std::function<void(std::stop_token)>&& func);

    // Waits for the thread to end. No-op without one.
    void Join();
    bool Joinable() const { return thread != nullptr; }

    // Requests a stop, joins and resets, so Run may start a new thread.
    void Stop();

private:
    static void* Entry(void* self);

    bb::hle::kernel::Thread* thread = nullptr;
    std::function<void(std::stop_token)> func;
    std::stop_source stop;
};

}  // namespace Libraries::Kernel

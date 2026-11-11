// LeakSanitizer's suppressions for the Vulkan test binaries (bbvktest). Mesa allocates its
// one-time state under pthread_once and never frees it. The loader has unloaded the driver by
// the time the leak check runs, so its frames have no module name to match. Once a test draws,
// lavapipe's worker threads also allocate state that hangs off those one-time blocks; their stacks
// end in the thread start (their frames are unnamed too). A narrower rule is not possible: the
// leaks are four 56-byte blocks allocated by the driver's own code, and with the driver unloaded
// LeakSanitizer sees no function or module name in them, only asan_thread_start at the bottom.
// The workers belong to the driver's global state and live until the process ends, so joining
// them from the test is not possible either.
extern "C" const char* __lsan_default_suppressions() {
    return "leak:pthread_once\nleak:asan_thread_start\n";
}

// LeakSanitizer's suppressions for the Vulkan test binaries (bbvktest). Mesa allocates its
// one-time state under pthread_once and never frees it. The loader has unloaded the driver by
// the time the leak check runs, so its frames have no module name to match.
extern "C" const char* __lsan_default_suppressions() {
    return "leak:pthread_once\n";
}

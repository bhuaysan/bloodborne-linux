// AddressSanitizer options for the test binaries whose tests map guest memory. The guest window
// lies in ASan's shadow gap, which ASan keeps PROT_NONE unless protect_shadow_gap=0
// (docs/design/m3-core-os.md, Memory, "Guest window"). The harness tests' main
// (tests/harness/main.cpp) and bb-run set the same option.
extern "C" const char* __asan_default_options() {
    return "protect_shadow_gap=0";
}

# Runtime design (M2)

The interfaces that the loader, the import stubs, the hooks and the differential test harness share. Issues #43 to #47 build on this note. Change it in the same PR when an implementation has to deviate.

## Source layout

```
src/
  common/    bbcommon   logging, errors, file reading, SHA-256, game directory lookup
  loader/    bbloader   SELF → ELF, ELF parsing, mapping, relocation, eh_frame lookup
  hle/       bbhle      HLE module registry, import stubs, NID name table
  hook/      bbhook     function-entry patches
  game/      bbgame     decompiled functions (registered into bbhook), the image accessor
  harness/   bbharness  differential test harness (tests only)
  tools/                small command-line programs (bb-load)
tests/<target>/         GoogleTest unit tests, one directory per library
```

Dependencies point one way:

```
common ← loader ← hle, hook ← game ← harness
```

The loader resolves imports through a callback (see [Relocations](#relocations)), so it never calls into `hle`, and the loader tests can bind imports to fakes.

All code is in namespace `bb` with a nested namespace per library (`bb::loader`, `bb::hle`, …).

### Static registration

HLE implementations (`bbhle` and later modules) and replacements (`bbgame`) register themselves into registries owned by `bbhle` and `bbhook`, through static objects in their source files. A static archive would drop object files that nothing references, and their registrations with them. Targets that contain registrations are therefore CMake `OBJECT` libraries. Every registry is a function-local static (construct on first use), so the order of static initialization across files doesn't matter.

## Addresses

- **Link address**: an address in the ELF as built, image base 0. `symbols/functions.csv`, Ghidra and every `// 0x…` comment use link addresses.
- **Runtime address**: base + link address.

Code that names a game address always takes a link address and converts it through the loaded image. Nothing stores runtime addresses in `symbols/` or in source.

## Loading

### Input

`map_eboot` reads `eboot.bin`, checks its SHA-256 against the hash in `docs/clean-room.md`, and rebuilds the ELF in memory (format in `docs/formats/self.md`). A unit test compares the rebuilt ELF's SHA-256 with the pinned `eboot.elf` hash in `tools/common/ps4elf.py`, which checks the C++ port against `tools/self2elf`.

The game directory comes from `BB_GAME_DIR` in the environment. Tests also read `local.env` from the **main checkout**, found the same way as `tools/common/toolenv.py` does: CMake runs `git rev-parse --git-common-dir` at configure time. A linked worktree has no `local.env` of its own, so reading it from the worktree root would skip every game test there. `bb::test::game_dir()` in `tests/support/` implements this. Tests that need the game use `BB_REQUIRE_GAME`, which calls `GTEST_SKIP()` when neither source sets the directory.

### Base and mapping

Default base: **0x10000000**. The eboot spans about 87 MiB (code segment at link address 0, data segment at 0x50dc000, `.bss` up to 0x56d30f4), so it ends well below 0x7fff8000. That matters for the `debug` preset: AddressSanitizer on x86-64 Linux keeps its shadow memory from 0x7fff8000 up to about 0x10007fff8000, so a mapping must lie either below 0x7fff8000 or above that range. The bundled PRXs (M3) go after the eboot, in the same low range.

The base is a parameter. It must be aligned to the largest `p_align` of the `PT_LOAD` segments (0x4000 in the eboot). The loader reserves the whole span with one `mmap(PROT_NONE, MAP_FIXED_NOREPLACE)` and checks that the returned address equals the requested one (kernels before 4.17 treat the flag as a hint). If the range is taken, loading fails with an error naming the range. It never moves silently. The segments are then made usable inside the reservation with `mprotect`. A second `MAP_FIXED_NOREPLACE` mapping there would fail, because the range is already mapped.

### API and steps

```cpp
namespace bb::loader {
inline constexpr uintptr_t default_base = 0x10000000;

Image map_eboot(const std::filesystem::path& game_dir, uintptr_t base = default_base);
Image map_elf(std::span<const std::byte> elf, uintptr_t base);  // any PS4 ELF, for tests

using Resolver = std::function<uintptr_t(const Import&)>;
void relocate(Image& image, const Resolver& resolve);
}
```

1. `map_eboot` / `map_elf`: reserve the span, make each `PT_LOAD` read-write and copy its file bytes. The rest up to `p_memsz` stays zero. Parse the dynamic data.
2. `relocate`: apply every relocation. The eboot has `DT_TEXTREL`, so the code segment must still be writable here.
3. `bb::hook::apply`: game runtime only, never in the harness.
4. `Image::finalize_protection()`: set the final protections from `p_flags`: code `r-x`, data `rw-`.

Who runs which steps:

| Caller | Steps |
|---|---|
| `bb-load` (M2) | 1, 2 with `bb::hle::resolve`, 4 |
| differential harness (M2) | 1, 2 with `bb::hle::resolve`, 4 |
| game runtime (M3) | 1, 2 with `bb::hle::resolve`, 3 with all registered replacements, 4, then TLS setup and the entry point |
| loader unit tests | 1, 2 with a fake resolver, 4 |

### Loaded image

```cpp
namespace bb::loader {
struct Segment {
    uint64_t link_addr, mem_size, file_size;
    int prot;                     // final PROT_* from p_flags
};
struct Import {
    std::string nid;              // 11 characters
    std::string library, module;  // names from the library and module records
    uint8_t type;                 // STT_FUNC or STT_OBJECT
};
struct Image {
    uintptr_t base;
    std::vector<Segment> segments;            // PT_LOAD only
    uint64_t entry;                           // link address
    std::optional<TlsTemplate> tls;           // PT_TLS: link addr, file size, mem size, align
    std::optional<uint64_t> proc_param;       // link address of PT_SCE_PROCPARAM, eboot only
    DynamicInfo dyn;                          // init/fini arrays, symbols, relocations, records
    uintptr_t runtime(uint64_t link) const;   // base + link, checked against the segments
    template <class F> F* fn(uint64_t link) const;
    std::optional<uint64_t> function_size(uint64_t link) const;  // see Hooks
    bool finalized() const;
    void finalize_protection();
};
}
```

The image owns its mapping and unmaps it on destruction. `DynamicInfo` holds the parsed tables from `PT_SCE_DYNLIBDATA` (`docs/formats/elf-dynamic.md`). Only the parsed data is kept, not the raw segment.

### Relocations

Supported: `R_X86_64_RELATIVE` (base + addend), `R_X86_64_64` (symbol + addend), `R_X86_64_GLOB_DAT` and `R_X86_64_JUMP_SLOT` (symbol). Any other type is an error that names the type and the offset. `DTPMOD64` and the TLS types wait for M3 (they don't occur in the eboot).

A relocation against a defined symbol of the same file (`st_shndx != 0`) resolves to base + `st_value` without calling the resolver. The eboot has none, but the PRXs (M3) and synthetic test ELFs do. Only undefined symbols are imports and go to the resolver.

Every import must resolve to a non-zero address. The game runtime, `bb-load` and the harness pass `bb::hle::resolve`.

## Imports and stubs

### Registry

```cpp
namespace bb::hle {
void register_function(std::string_view library, std::string_view nid, void* impl);
uintptr_t resolve(const loader::Import& imp);  // impl, else stub
void set_stub_observer(StubObserver* observer); // nullptr restores logging
}
```

Implementations register by library name and NID, through a static-registration macro in each module's source file. `resolve` returns the registered implementation, or else a stub. It is idempotent: each (library, NID) gets exactly one stub thunk or data block, created on the first call and cached. The eboot has thousands of relocations against a few hundred imports, and function-pointer equality and the per-import call counters depend on getting the same address every time.

### Function stubs

Each unresolved function import gets its own thunk in an executable page that the HLE layer owns:

```
mov  r11, imm64      ; 49 BB imm64: pointer to this import's StubInfo
jmp  [rip+0]         ; FF 25 00000000: absolute jump through the next 8 bytes
.quad stub_entry
```

That is 24 bytes per thunk. `r11` is a scratch register in the System V ABI and never carries an argument. `stub_entry` (assembly) saves the six integer argument registers, keeps the stack 16-byte aligned, calls `bb::hle::on_stub_call(const StubInfo*, const uint64_t args[6])`, and returns with `rax = 0`, `xmm0 = 0`.

`StubInfo` holds the library, the module, the NID, the name from `symbols/nids.csv` (or `?`) and a call counter. `on_stub_call` logs all of these plus the six integer arguments. It logs the first 10 calls per import and then every 1000th. When an observer is set, it receives the call instead of the log.

The NID name table is generated at build time from `symbols/nids.csv` by a small Python script into a sorted array that is searched with binary search. CMake reruns the script when the CSV changes.

### Data imports

Each unresolved data import (`STT_OBJECT`) gets its own zero-filled 4 KiB block, also cached per (library, NID). `R_X86_64_64` relocations against imported data use small offsets (`symbol + 0x10`, see `docs/formats/elf-dynamic.md`), which stay inside the block. The harness compares these blocks like the data segment.

## Hooks

```cpp
namespace bb::hook {
struct Replacement { uint64_t link_addr; void* impl; const char* name; };
std::span<const Replacement> replacements();                    // all registered
void apply(loader::Image&, std::span<const Replacement>);
void remove(loader::Image&, std::span<const Replacement>);
}
#define BB_REPLACE(link_addr, function) …                        // static registration
```

The patch at a function entry is 14 bytes: `jmp [rip+0]` (`FF 25 00000000`) followed by the absolute address of the replacement.

**Size check.** `apply` asks `Image::function_size(link)`, which looks the address up in the binary search table of `PT_GNU_EH_FRAME` (`docs/formats/eh-frame.md`, dead FDEs excluded) and returns the FDE's range length only when an FDE starts exactly at that address. `apply` refuses an address that is not an FDE start and a function shorter than 14 bytes. `symbols/functions.csv` has no size column, so the FDE is the only source. #45 adds `function_size` to the loader.

**Protection.** In the runtime, hooks are applied before `finalize_protection`, while the code is still writable. When the image is already finalized (tests, `remove`), `apply` and `remove` make the affected pages writable themselves and restore `r-x` afterwards. Each patched entry keeps its original bytes for `remove`.

**Calling the game.** A replacement calls a game function that isn't replaced yet through its link address, using the image accessor in `bbgame`:

```cpp
namespace bb::game {
loader::Image& image();               // the loaded eboot; aborts if none is set
void set_image(loader::Image* image); // called by the game runtime and the harness after loading
}
// in a replacement:
bb::game::image().fn<void(Foo*)>(0x123450)(foo);
auto* flags = reinterpret_cast<uint32_t*>(bb::game::image().runtime(0x5100000));
```

The game code and our code use the same System V calling convention, so a call needs no glue as long as no TLS is involved. From M3 the game uses `%fs` for its own TLS while our code uses glibc's, so transitions between the two will need glue. That is open until M3 sets up TLS.

**Calling imports.** A replacement calls an imported function through the address that `bb::hle::resolve` returns for that import (the same address the loader wrote into the image's GOT slot), never an HLE C++ function directly. Then the original and the replacement reach the same stub or implementation, and the stub logs compare.

## Differential test harness

### Process setup

One GoogleTest environment per test binary loads the eboot once: the default base, `relocate` with `bb::hle::resolve`, `finalize_protection`, and `bb::game::set_image`. Imports with a registered HLE implementation call it; all others go to stubs, and the harness installs a stub observer that records those calls. **Hooks are never applied in the harness.** It calls the original at its runtime address and the replacement as a C++ function.

No game initialization code runs: not `DT_INIT`, `DT_PREINIT_ARRAY`, `DT_INIT_ARRAY` nor the entry point. The baseline is the static image right after relocation. A function that depends on state set up by init code needs that state built by its test's input builder.

After loading, the harness takes a **baseline snapshot** of all writable memory it owns:
- the image's writable segments (data and `.bss`, about 6 MiB),
- the data-import blocks,
- the **arena**, an mmap'd region of 16 MiB that the harness owns for test inputs.

### One comparison

```cpp
auto r = harness.compare<int(const char*, size_t)>(
    0x00412a30, &bb::game::string_hash,
    [](Arena& a) { return std::tuple{a.str("abc"), size_t{3}}; });
EXPECT_TRUE(r) << r.report();
```

1. Restore the baseline, build the inputs (the builder runs before each call, so both calls see the same arena layout), clear the stub log, call the **original**, capture its return value, all snapshot regions and the stub log.
2. Restore the baseline, build the same inputs, call the **replacement**, capture the same.
3. Compare:
   - the return value: integers and pointers bit for bit, floating point bit for bit (two NaNs with different payloads differ), structs returned in registers as `rax:rdx` / `xmm0:xmm1`,
   - every byte of the snapshot regions. The report lists the first differing ranges, as link addresses for the image and arena offsets for the arena,
   - the stub log: same imports in the same order with the same integer arguments. Calls to registered HLE implementations are not logged or compared. Pointer arguments are normalized before comparing: a pointer into a snapshot region compares by region and offset, a pointer into the calling thread's stack compares only as "a stack pointer" (the original and the replacement lay out their frames differently), any other value compares as is.

Not compared: the stack below the call, registers other than the return registers, the code segment.

### Limits for M2

- No TLS: functions that read `%fs` are out of scope until M3 sets up TLS. The harness doesn't set `%fs`, so such a function reads Linux's TLS block and the comparison is meaningless.
- No heap: an import such as `malloc` is a stub that returns 0. Functions that allocate wait for the HLE allocator (M3), which will draw from the arena so its memory is compared too.
- A crash in the original or the replacement kills the test binary. Isolating each call in a forked child can come later if it is needed.
- The original code isn't instrumented by AddressSanitizer. Only the replacement and the harness are.
- AddressSanitizer's stack-use-after-return detection, on by default, moves an instrumented function's locals into fake stack frames on the heap. Pointers to the replacement's locals would then not look like stack pointers and would compare unequal. The harness turns it off with `__asan_default_options()` returning `detect_stack_use_after_return=0`, defined in the harness's GoogleTest main so the linker keeps it.

## Tools

`bb-load` maps the eboot, binds every import through `bb::hle::resolve`, prints the base, the segments, the number of relocations by type and the number of imports by library, and exits without running game code. Its output is checked against `tools/elfimports`.

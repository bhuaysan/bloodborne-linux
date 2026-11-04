# Runtime design (M2)

The interfaces that the loader, the import stubs, the hooks and the differential test harness share. Issues #43 to #47 build on this note. Change it in the same PR when an implementation has to deviate.

## Source layout

```
src/
  common/    bbcommon   logging, errors, file reading, SHA-256, game directory lookup
  loader/    bbloader   SELF → ELF, ELF parsing, mapping, relocation, import binding
  hle/       bbhle      HLE module registry, import stubs, NID name table
  hook/      bbhook     function-entry patches
  game/      bbgame     decompiled functions and their replacement table
  harness/   bbharness  differential test harness (tests only)
  tools/                small command-line programs (bb-load)
tests/<target>/         GoogleTest unit tests, one directory per library
```

Dependencies point one way: `common` ← `loader`, `hle`, `hook` ← `game` ← `harness`. `loader` depends on `hle` only through the resolver callback below, so the loader tests can bind imports to fakes.

All code is in namespace `bb` with a nested namespace per library (`bb::loader`, `bb::hle`, …).

## Addresses

- **Link address**: an address in the ELF as built, image base 0. `symbols/functions.csv`, Ghidra and every `// 0x…` comment use link addresses.
- **Runtime address**: base + link address.

Code that names a game address always takes a link address and converts it through the loaded image. Nothing stores runtime addresses in `symbols/` or in source.

## Loading

### Input

`bb::loader::load_eboot(game_dir)` reads `eboot.bin`, checks its SHA-256 against the hash in `docs/clean-room.md`, and rebuilds the ELF in memory (format in `docs/formats/self.md`). A unit test compares the rebuilt ELF's SHA-256 with the pinned `eboot.elf` hash in `tools/common/ps4elf.py`, which checks the C++ port against `tools/self2elf`.

The game directory comes from `BB_GAME_DIR`. Tests also read `local.env` in the repository root, whose path CMake passes in. Tests that need the game call `GTEST_SKIP()` when neither is set.

### Base

Default base: **0x10000000**. The eboot spans about 87 MiB (code segment at link address 0, data segment at 0x50dc000, `.bss` up to about 0x56d3000), so it ends well below 0x7fff8000. That matters for the `debug` preset: AddressSanitizer on x86-64 Linux reserves shadow memory from 0x7fff8000 up to about 0x10007fff8000, so a mapping must lie either below 0x7fff8000 or above that range. The bundled PRXs (M3) go after the eboot, in the same low range.

The base is a parameter of the loader, so tests can pick another one. The whole image span is reserved with one `mmap(PROT_NONE, MAP_FIXED_NOREPLACE)`. If the range is taken, loading fails with an error naming the range. It never moves silently.

### Loaded image

```cpp
namespace bb::loader {
struct Segment {
    uint64_t link_addr, mem_size, file_size;
    int prot;                     // PROT_* from p_flags
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
    uint64_t proc_param;                      // link address of PT_SCE_PROCPARAM
    DynamicInfo dyn;                          // init/fini arrays, symbols, relocations, records
    uintptr_t runtime(uint64_t link) const;   // base + link, checked against the segments
    template <class F> F* fn(uint64_t link) const;
};
}
```

The image owns its mapping and unmaps it on destruction. `DynamicInfo` holds the parsed tables from `PT_SCE_DYNLIBDATA` (`docs/formats/elf-dynamic.md`). Only the parsed data is kept, not the raw segment.

### Steps and permissions

1. Reserve the span, then map each `PT_LOAD` read-write and copy its file bytes. The rest up to `p_memsz` stays zero.
2. Apply relocations. The eboot has `DT_TEXTREL`, so the code segment must be writable here.
3. Apply hooks (game runtime only, never in the harness).
4. Set the final protections from `p_flags`: code `r-x`, data `rw-`.

### Relocations

Supported: `R_X86_64_RELATIVE` (base + addend), `R_X86_64_64` (symbol + addend), `R_X86_64_GLOB_DAT` and `R_X86_64_JUMP_SLOT` (symbol). Any other type is an error that names the type and the offset. `DTPMOD64` and the TLS types wait for M3 (they don't occur in the eboot).

The loader resolves symbols through a callback, so it doesn't know about HLE:

```cpp
using Resolver = std::function<uintptr_t(const Import&)>;
```

Every import must resolve to a non-zero address. The game runtime and the harness pass `bb::hle::resolve`.

## Imports and stubs

### Registry

```cpp
namespace bb::hle {
void register_function(std::string_view library, std::string_view nid, void* impl);
uintptr_t resolve(const loader::Import& imp);  // impl, else stub
}
```

Implementations register by library name and NID, through a static-registration macro in each module's source file. `resolve` returns the registered implementation, or else a stub.

### Function stubs

Each unresolved function import gets its own thunk in an executable page that the HLE layer owns:

```
mov  r11, imm64      ; pointer to this import's StubInfo
jmp  [rip+0]         ; absolute jump to stub_entry
.quad stub_entry
```

`r11` is a scratch register in the System V ABI and never carries an argument. `stub_entry` (assembly) saves the six integer argument registers, keeps the stack 16-byte aligned, calls `bb::hle::on_stub_call(const StubInfo*, const uint64_t args[6])`, and returns with `rax = 0`, `xmm0 = 0`.

`on_stub_call` logs the library, the NID, the name from `symbols/nids.csv` (or `?`) and the six integer arguments. It logs the first 10 calls per import and then every 1000th, with a call counter. The harness swaps in a recorder through `bb::hle::set_stub_observer`.

The NID name table is generated at build time from `symbols/nids.csv` by a small Python script into a sorted array that is searched with binary search. CMake reruns the script when the CSV changes.

### Data imports

Each unresolved data import (`STT_OBJECT`) gets its own zero-filled 4 KiB block. `R_X86_64_64` relocations against imported data use small offsets (`symbol + 0x10`, see `docs/formats/elf-dynamic.md`), which stay inside the block. The harness compares these blocks like the data segment.

## Hooks

```cpp
namespace bb::hook {
struct Replacement { uint64_t link_addr; void* impl; const char* name; };
std::span<const Replacement> replacements();          // all registered
void apply(loader::Image&, std::span<const Replacement>);
}
#define BB_REPLACE(link_addr, function) …              // static registration
```

The patch at a function entry is 14 bytes: `jmp [rip+0]` followed by the absolute address of the replacement. `apply` refuses a function shorter than 14 bytes, with the size taken from the eh_frame FDE of that address (`docs/formats/eh-frame.md`). Each patched entry keeps its original bytes, so a hook can be removed again. Hooks are applied before the final `mprotect` (step 3 above).

A replacement calls a game function that isn't replaced yet through its link address: `image.fn<void(Foo*)>(0x123450)(foo)`. The game code and our code use the same System V ABI, so no glue is needed.

## Differential test harness

### Process setup

One GoogleTest environment per test binary loads the eboot once: the default base, relocations, and every import bound to a recording stub. **Hooks are never applied in the harness.** It calls the original at its runtime address and the replacement as a C++ function.

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
   - the stub log: same imports in the same order with the same integer arguments. Pointer arguments that point into a snapshot region are compared by offset, not by address.

Not compared: the stack below the call, registers other than the return registers, the code segment.

### Limits for M2

- No TLS: functions that read `%fs` are out of scope until M3 sets up TLS. The harness doesn't set `%fs`, so such a function reads Linux's TLS block and the comparison is meaningless.
- No heap: an import such as `malloc` is a stub that returns 0. Functions that allocate wait for the HLE allocator (M3), which will draw from the arena so its memory is compared too.
- A crash in the original or the replacement kills the test binary. Isolating each call in a forked child can come later if it is needed.
- The original code isn't instrumented by AddressSanitizer. Only the replacement and the harness are.

## Tools

`bb-load` maps the eboot, binds every import to a stub, prints the base, the segments, the number of relocations by type and the number of imports by library, and exits without running game code. Its output is checked against `tools/elfimports`.

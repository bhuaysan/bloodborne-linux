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
tests/support/          bbtestsupport: shared test helpers (game directory lookup, BB_REQUIRE_GAME)
```

Dependencies point one way:

```
common ← loader ← hle, hook ← game ← harness
```

The loader resolves imports through a callback (see [Relocations](#relocations)), so it never calls into `hle`, and the loader tests can bind imports to fakes.

All code is in namespace `bb` with a nested namespace per library (`bb::loader`, `bb::hle`, …).

### Static registration

HLE implementations (`bbhle` and later modules) and replacements (`bbgame`) register themselves into registries owned by `bbhle` and `bbhook`, through static objects in their source files. A static archive would drop object files that nothing references, and their registrations with them. Targets that contain registrations are therefore CMake `OBJECT` libraries, and only executables link them, directly. `bbharness` is a `STATIC` library that takes only the compile-time usage requirements (include directories) of `bbgame` and `bbhle`, for example through `$<COMPILE_ONLY:…>`, never their objects; each test executable links `bbgame` and `bbhle` itself. A `STATIC` library that links an `OBJECT` library puts its objects into the archive again, where the final link would drop them silently. Every registry is a function-local static (construct on first use), so the order of static initialization across files doesn't matter.

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

Every executable is built as PIE (`CMAKE_POSITION_INDEPENDENT_CODE` in the top-level `CMakeLists.txt`). The kernel puts the heap (`brk`) of a non-PIE program at a random address up to 1 GiB above the program at 0x400000, so some runs found the eboot's range taken. A PIE program and its heap lie far above 0x7fff8000.

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
2. `relocate`: apply every relocation. The eboot sets `DT_TEXTREL`. None of its relocations actually target the code segment, but the flag allows it, so the code segment stays writable until step 4.
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
    std::optional<UnwindTable> unwind;        // PT_GNU_EH_FRAME search table, see Hooks
    uintptr_t runtime(uint64_t link) const;   // base + link, checked against the segments
    template <class F> F* fn(uint64_t link) const;
    std::optional<uint64_t> function_size(uint64_t link) const;  // see Hooks
    bool finalized() const;
    void finalize_protection();
    Reservation mapping;                      // the reserved range, unmapped on destruction
};
}
```

`PT_TLS` and `PT_SCE_PROCPARAM` are optional, at most one of each. For `PT_TLS`, `p_filesz` must not exceed `p_memsz`, and `p_align` must be a power of two; `p_align` 0 counts as 1 (no constraint), as for any ELF segment, so `TlsTemplate::align` is never 0. Only the initialization image (`p_vaddr` + `p_filesz`) has to lie inside a `PT_LOAD`: the rest of the block is zero-filled per thread and needs no memory in the image. `PT_SCE_PROCPARAM` (`p_vaddr` + `p_memsz`) must lie inside a `PT_LOAD`. Both are checked before the range is reserved. In the eboot, the TLS template is at 0x50dc040 with `p_filesz` 0, `p_memsz` 0x750 and `p_align` 0x10, and the process parameters are at 0x50dc000, the start of the data segment.

The image owns its mapping through `mapping` and unmaps it on destruction. It is move-only; a move keeps every runtime address valid. `DynamicInfo` holds the parsed tables from `PT_SCE_DYNLIBDATA` (`docs/formats/elf-dynamic.md`): the symbols, both relocation tables, the module and library records, the `Import` of each undefined `NID#library#module` symbol, and the values of `DT_SCE_PLTGOT`, `DT_INIT`, `DT_FINI`, the preinit, init and fini arrays (link address and size), `DT_NEEDED` (in order) and `DT_TEXTREL`. The records are checked as `tools/elfimports` checks them. Only the parsed data is kept, not the raw segment.

### Relocations

Supported: `R_X86_64_RELATIVE` (base + addend), `R_X86_64_64` (symbol + addend), `R_X86_64_GLOB_DAT` and `R_X86_64_JUMP_SLOT` (symbol). Any other type is an error that names the type and the offset. `DTPMOD64` and the TLS types wait for M3 (they don't occur in the eboot).

A relocation against a defined symbol of the same file (`st_shndx != 0`) resolves to base + `st_value` without calling the resolver. The eboot has none, but the PRXs (M3) and synthetic test ELFs do. Undefined symbols with a `NID#library#module` name are imports and go to the resolver. The weak, untyped, undefined `module_start` and `module_stop` of the PRXs (`docs/formats/elf-dynamic.md`) are not imports: relocations against them resolve to 0, as for any unresolved weak symbol. Any other plain-named undefined symbol is an error.

Every import must resolve to a non-zero address. The game runtime, `bb-load` and the harness pass `bb::hle::resolve`.

`relocate` (declared in `loader/relocate.hpp`) applies `DT_SCE_RELA` first, then `DT_SCE_JMPREL`. It checks the type, then the target, then resolves the symbol, so the resolver never sees an import of a rejected relocation. Each 8-byte target must lie inside the memory of a `PT_LOAD`, and inside a writable one unless the image has `DT_TEXTREL` (checked without overflow). On an error the image is partly relocated and must be discarded. Calling `relocate` after `finalize_protection` is a `std::logic_error`. In the eboot, no relocation targets the code segment, 686 of the 701 imports are referenced, and `R_X86_64_64` against imported data uses only the addends 0 and 0x10.

## Imports and stubs

### Registry

```cpp
namespace bb::hle {
void register_function(std::string_view library, std::string_view nid, void* impl);
uintptr_t resolve(const loader::Import& imp);  // impl, else stub
uintptr_t import_address(std::string_view library, std::string_view nid);  // cached result of resolve
void set_stub_observer(StubObserver* observer); // nullptr restores logging
Counts counts();                                // stubs and data blocks created so far
}
#define BB_HLE_FUNCTION(library, nid, function) …  // static registration
```

Implementations register by library name and NID, through `BB_HLE_FUNCTION` at namespace scope in each module's source file. Registering an import twice, or after it was resolved, is a `std::logic_error`. `resolve` returns the registered implementation, or else a stub. It is idempotent: each (library, NID) gets exactly one stub thunk or data block, created on the first call and cached. The eboot has thousands of relocations against a few hundred imports, and function-pointer equality and the per-import call counters depend on getting the same address every time.

### Function stubs

Each unresolved function import gets its own thunk in an executable page that the HLE layer owns:

```
mov  r11, imm64      ; 49 BB imm64: pointer to this import's StubInfo
jmp  [rip+0]         ; FF 25 00000000: absolute jump through the next 8 bytes
.quad stub_entry
```

That is 24 bytes per thunk. `r11` is a scratch register in the System V ABI and never carries an argument. `stub_entry` (assembly) saves the six integer argument registers, keeps the stack 16-byte aligned, calls `bb::hle::on_stub_call(const StubInfo*, const uint64_t args[6])`, and returns with `rax = 0`, `xmm0 = 0`.

`StubInfo` holds the library, the module, the NID, the name from `symbols/nids.csv` (or `?`) and a call counter (atomic, incremented before the observer or the log sees the call). `StubObserver` is an interface with `virtual void on_call(const StubInfo&, const uint64_t args[6])`, so an observer can keep its own state, as the harness's recorder does. `on_stub_call` has C++ linkage: `stub_entry.S` calls it by its mangled name, so a change of its signature fails at link time instead of passing arguments wrongly. Only `bb_hle_stub_entry` itself, an assembly symbol, is declared `extern "C"`.

`on_stub_call` logs all of these plus the six integer arguments. It logs the first 10 calls per import and then every 1000th. When an observer is set, it receives the call instead of the log.

The thunks live in anonymous pages of `bbhle`. A thunk is written while its page is `rw-`, and the page goes back to `r-x` right after. That is safe as long as `resolve` runs before game code; resolving while game threads may execute an earlier thunk of the same page (lazy PRX loading in M3) needs another scheme, for example a second, writable mapping of the same pages. Thunks, `StubInfo` objects and data blocks are never freed.

The NID name table is generated at build time from `symbols/nids.csv` by a small Python script into a sorted array that is searched with binary search. CMake reruns the script when the CSV changes. `bb::hle::nid_name(nid)` returns the name, or `?` for a NID the table doesn't have.

### Data imports

Each unresolved data import (`STT_OBJECT`) gets its own zero-filled 4 KiB block, also cached per (library, NID). `R_X86_64_64` relocations against imported data use small offsets (`symbol + 0x10`, see `docs/formats/elf-dynamic.md`), which stay inside the block. The harness compares these blocks like the data segment.

## Hooks

```cpp
namespace bb::hook {
struct Replacement { uint64_t link_addr; void* impl; const char* name; };
std::span<const Replacement> replacements();                    // all registered
void register_replacement(const Replacement&);                  // used by BB_REPLACE
void apply(loader::Image&, std::span<const Replacement>);
void remove(loader::Image&, std::span<const Replacement>);
}
#define BB_REPLACE(link_addr, function) …                        // static registration
```

The patch at a function entry is 14 bytes: `jmp [rip+0]` (`FF 25 00000000`) followed by the absolute address of the replacement.

**Size check.** `apply` asks `Image::function_size(link)`, which looks the address up in the binary search table of `PT_GNU_EH_FRAME` (`docs/formats/eh-frame.md`, dead FDEs excluded) and returns the FDE's range length only when an FDE starts exactly at that address. `apply` refuses an address that is not an FDE start and a function shorter than 14 bytes. `symbols/functions.csv` has no size column, so the FDE is the only source. #45 adds `function_size` to the loader.

The table is untrusted input, checked in two places:
- **At map time** (`read_unwind_table`, before the range is reserved): at most one `PT_GNU_EH_FRAME`, inside the file data of a readable `PT_LOAD`; the header encodings of every pinned file (version 1, `eh_frame_ptr` pcrel|sdata4, `fde_count` udata4, table datarel|sdata4); the table fits `p_filesz` and its starts ascend strictly. Anything else is a `LoadError`, so an image with an unreadable table never maps. An image without `PT_GNU_EH_FRAME` maps, and `function_size` returns nothing for it.
- **At lookup** (`fde_range`): a binary search over the table. No entry that starts exactly at the address gives `nullopt`. An entry whose FDE or CIE is malformed, lies outside that segment's file data, uses an augmentation other than `z` + `R`/`L`/`P`, an FDE pointer encoding other than pcrel with a 4- or 8-byte format, or whose decoded `pc_begin` differs from the table's start, throws `LoadError`: the table points at broken data, which is not the same as "no function here".

**Protection.** In the runtime, hooks are applied before `finalize_protection`, while the code is still writable. When the image is already finalized (tests, `remove`), `apply` and `remove` make the affected pages (`page_down(entry)` to `page_up(entry + 14)`) writable themselves and give them their segment's final protection (`r-x`) back afterwards.

**Checks and saved bytes.** `apply` and `remove` check every entry of the span before they write anything, and throw `bb::hook::HookError` naming the replacement, the link address and the reason; a refused span patches nothing. Besides the size check, `apply` refuses a null `impl`, a patch that doesn't lie inside an executable segment, and a patch whose 14 bytes overlap those of another entry in the span or of a live hook (double hook). FDE ranges may overlap in untrusted unwind data, so a distinct FDE start alone doesn't rule this out. `remove` refuses an entry that isn't hooked; a refused span restores nothing.

The original 14 bytes of each patched entry live in a registry in `bbhook`, keyed by (image base, link address), next to the patch that was written. `Image` stays free of hook state, and the loader doesn't depend on `bbhook`. An image that is destroyed with hooks still applied leaves its entries behind, and a later image may be mapped at the same base. So an entry counts as hooked only while its patch is still in memory; one whose patch is gone is dropped. `apply` and `remove` are not thread-safe: they run while no other thread executes game code.

**Registration.** `BB_REPLACE(0x123450, my_function);` at namespace scope defines a static `bb::hook::Registrar` in an anonymous namespace, whose constructor calls `register_replacement` with the link address, `&my_function` and the name `"my_function"`. The registry behind `replacements()` is a function-local static vector in `bbhook`, in registration order; it checks nothing, `apply` does. `bbhook` is a `STATIC` library because it owns the registry but registers nothing itself ("Static registration").

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

**Calling imports.** A replacement calls an imported function through `bb::hle::import_address(library, nid)`, which returns the cached address that `resolve` handed out for that import (the same address the loader wrote into the image's GOT slot) and aborts if the import was never resolved. It never calls an HLE C++ function directly. Then the original and the replacement reach the same stub or implementation, and the stub logs compare.

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
   - the return value as the typed `R` object: integers and pointers bit for bit, floating point bit for bit (two NaNs with different payloads differ), structs through `operator==`, never as raw bytes, because padding is indeterminate. A defaulted `operator==` compares float members by value (NaN ≠ NaN, +0 = −0); a test that needs bitwise floats writes its own,
   - every byte of the snapshot regions. The report lists the first differing ranges, as link addresses for the image and arena offsets for the arena,
   - the stub log: same imports in the same order. Arguments are compared only up to the arity the test declares for an import (`harness.declare_arity(library, nid, n)`), default none, because the arity of an unnamed import is unknown and unused argument registers hold leftovers that differ between the two compiled versions. All six are recorded and shown in the report. Calls to registered HLE implementations are not logged or compared. Compared pointer arguments are normalized first: a pointer into a snapshot region compares by region and offset, a pointer into the calling thread's stack compares only as "a stack pointer" (the original and the replacement lay out their frames differently), any other value compares as is.

Not compared: the stack below the call, registers other than the return registers, the code segment.

### Limits for M2

- No TLS: functions that use `%fs` must not be called until M3 sets up TLS. The harness doesn't set `%fs`, so such a function would read and possibly overwrite glibc's own TLS block and thread control block, which can crash or corrupt the test process.
- HLE implementations called from a diff test must be stateless, or keep their state in a snapshot region. Other state isn't restored between the two calls, and calls to implementations aren't compared.
- No heap: an import such as `malloc` is a stub that returns 0. Functions that allocate wait for M3, where `malloc` comes either from an HLE allocator or from the loaded `libc.prx`. Either way its memory must come from the arena so it is compared too.
- A crash in the original or the replacement kills the test binary. Isolating each call in a forked child can come later if it is needed.
- The original code isn't instrumented by AddressSanitizer. Only the replacement and the harness are.
- AddressSanitizer's stack-use-after-return detection, on by default, moves an instrumented function's locals into fake stack frames on the heap. Pointers to the replacement's locals would then not look like stack pointers and would compare unequal. The harness turns it off with `__asan_default_options()` returning `detect_stack_use_after_return=0`, defined in the harness's GoogleTest main so the linker keeps it.
- With clang, `-fsanitize=undefined` includes `-fsanitize=function`, which reads a signature word next to the target of every indirect call. Game code and stub thunks have none, so clang builds add `-fno-sanitize=function`. GCC has no such check.

## Tools

`bb-load` maps the eboot, binds every import through `bb::hle::resolve`, prints the base, the segments, the number of relocations by type and the number of imports by library, and exits without running game code. Its output is checked against `tools/elfimports`.

The game directory comes from `--game-dir` or else `BB_GAME_DIR` in the environment. Unlike the tests, `bb-load` doesn't read `local.env`: it is a user-facing program and runs outside the repository too. The #43 version maps and finalizes the eboot and prints the base, the entry point, the segments (link address, memory and file size, protection), the TLS template, the `PT_SCE_PROCPARAM` address and the imports by library (functions and objects, in the order of `tools/elfimports`). #44 adds the relocation step with `bb::hle::resolve`: `bb-load` also prints the number of relocations by type over both tables and the numbers of function stubs and data blocks that `resolve` created (666 and 20 for the eboot). A test runs it on the eboot and compares the counts with those of `tools/elfimports` for `eboot.elf`: 701 imports in 39 libraries.

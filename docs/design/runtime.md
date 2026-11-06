# Runtime design (M2)

The interfaces that the loader, the import stubs, the hooks and the differential test harness share. Issues #43 to #47 build on this note. Change it in the same PR when an implementation has to deviate.

## Source layout

```
src/
  common/    bbcommon   file reading, SHA-256, game directory lookup
  loader/    bbloader   SELF → ELF, ELF parsing, mapping, relocation, eh_frame lookup, load set
  hle/       bbhle      HLE module registry, import stubs, NID name table
  hook/      bbhook     function-entry patches
  game/      bbgame     decompiled functions (registered into bbhook), the image accessor
  harness/   bbharness  differential test harness (tests only)
  tools/                small command-line programs (bb-load, bb-gcn)
tests/<target>/         GoogleTest unit and differential tests, one directory per target
tests/support/          bbtestsupport: shared test helpers (game directory lookup, BB_REQUIRE_GAME)
tests/harness/          harness tests, plus bbeboottest: the EbootTest fixture (also used by tests/game/)
```

Dependencies point one way:

```
common ← loader ← hle, hook ← game
          loader ← harness (and the include directories of hle)
```

`bbharness` knows nothing of `bbgame`. Tests that call game functions through the harness link both: `bbharness` through `bbeboottest`, and `bbgame` directly ("Process setup").

The loader resolves imports through a callback (see [Relocations](#relocations)), so it never calls into `hle`, and the loader tests can bind imports to fakes.

All code is in namespace `bb` with a nested namespace per library (`bb::loader`, `bb::hle`, …).

### Static registration

HLE implementations (`bbhle` and later modules) and replacements (`bbgame`) register themselves into registries owned by `bbhle` and `bbhook`, through static objects in their source files. A static archive would drop object files that nothing references, and their registrations with them. Targets that contain registrations are therefore CMake `OBJECT` libraries, and only executables link them, directly. `bbharness` and `bbeboottest` are `STATIC` libraries that take only the compile-time usage requirements (include directories) of `bbhle`, and for `bbeboottest` also of `bbgame`, through `$<COMPILE_ONLY:…>` (which needs CMake 3.27, the project's minimum), never their objects; each test executable links `bbgame` and `bbhle` itself. A `STATIC` library that links an `OBJECT` library puts its objects into the archive again, where the final link would drop them silently. Every registry is a function-local static (construct on first use), so the order of static initialization across files doesn't matter.

## Addresses

- **Link address**: an address in the ELF as built, image base 0. `symbols/functions.csv`, Ghidra and every `// 0x…` comment use link addresses.
- **Runtime address**: base + link address.

Code that names a game address always takes a link address and converts it through the loaded image. Nothing stores runtime addresses in `symbols/` or in source.

## Loading

### Input

`map_eboot` reads `eboot.bin`, checks its SHA-256 against the hash in `docs/clean-room.md`, and rebuilds the ELF in memory (format in `docs/formats/self.md`). A unit test compares the rebuilt ELF's SHA-256 with the pinned `eboot.elf` hash in `tools/common/ps4elf.py`, which checks the C++ port against `tools/self2elf`.

The game directory comes from `BB_GAME_DIR` in the environment. Tests also read `local.env` from the **main checkout**, found the same way as `tools/common/toolenv.py` does: CMake runs `git rev-parse --git-common-dir` at configure time. A linked worktree has no `local.env` of its own, so reading it from the worktree root would skip every game test there. `bb::test::game_dir()` in `tests/support/` implements this. Tests that need the game use `BB_REQUIRE_GAME`, which calls `GTEST_SKIP()` when neither source sets the directory.

### Base and mapping

Default base: **0x10000000**. The eboot spans about 87 MiB (code segment at link address 0, data segment at 0x50dc000, `.bss` up to 0x56d30f4), so it ends well below 0x7fff8000. That matters for the `debug` preset: AddressSanitizer on x86-64 Linux keeps its shadow memory from 0x7fff8000 up to about 0x10007fff8000, so a mapping must lie either below 0x7fff8000 or above that range. The bundled PRXs (M3) go after the eboot, in the same low range: `libSceFios2.prx` at 0x156d4000 and `libc.prx` at 0x1584c000 with the default base (see Load set below).

Every executable is built as PIE (`CMAKE_POSITION_INDEPENDENT_CODE` in the top-level `CMakeLists.txt`). The kernel puts the heap (`brk`) of a non-PIE program at a random address up to 1 GiB above the program at 0x400000, so some runs found the eboot's range taken. A PIE program and its heap lie far above 0x7fff8000.

The base is a parameter. It must be aligned to the largest `p_align` of the `PT_LOAD` segments (0x4000 in the eboot). The loader reserves the whole span with one `mmap(PROT_NONE, MAP_FIXED_NOREPLACE)` and checks that the returned address equals the requested one (kernels before 4.17 treat the flag as a hint). If the range is taken, loading fails with an error naming the range. It never moves silently. The segments are then made usable inside the reservation with `mprotect`. A second `MAP_FIXED_NOREPLACE` mapping there would fail, because the range is already mapped.

### API and steps

```cpp
namespace bb::loader {
inline constexpr uintptr_t default_base = 0x10000000;

Image map_eboot(const std::filesystem::path& game_dir, uintptr_t base = default_base);
Image map_elf(std::span<const std::byte> elf, uintptr_t base);  // any PS4 ELF, for tests

using Resolver = std::function<uintptr_t(const Import&)>;
inline constexpr uint64_t eboot_module_id = 1;
void relocate(Image& image, const Resolver& resolve, uint64_t module_id);
}
```

1. `map_eboot` / `map_elf`: reserve the span, make each `PT_LOAD` read-write and copy its file bytes. The rest up to `p_memsz` stays zero. Parse the dynamic data.
2. `relocate`: apply every relocation. The eboot sets `DT_TEXTREL`. None of its relocations actually target the code segment, but the flag allows it, so the code segment stays writable until step 4.
3. `bb::hook::apply`: game runtime only, never in the harness.
4. `Image::finalize_protection()`: set the final protections from `p_flags`: code `r-x`, data `rw-`.

Who runs which steps:

| Caller | Steps |
|---|---|
| `bb-load` | 1 for the load set, 2 through `bb::hle::link`, 4 |
| differential harness (M2) | 1, 2 with `bb::hle::resolve`, 4 |
| game runtime (M3) | 1 for the eboot and the bundled PRXs, 2 through `bb::hle::link` (their exports registered with `bb::hle`, `bb::hle::resolve` for all three, then the PRXs' TLS templates registered with the TLS helper), 3 with all registered replacements, 4, the TLS check, then PRX init and the entry point on the game's main thread ([m3-core-os.md](m3-core-os.md)) |
| loader unit tests | 1, 2 with a fake resolver, 4 |

### Load set

`loader/load_set.hpp` (M3):

```cpp
namespace bb::loader {
inline constexpr std::array<std::string_view, 2> load_set_prxs = {"libSceFios2.prx", "libc.prx"};
struct Export { std::string library, nid; uintptr_t address; };
std::vector<Export> exports(const Image& image);  // defined NID#lib#0 symbols, STT_TLS skipped
Image map_prx(const std::filesystem::path& game_dir, std::string_view name, uintptr_t base);
struct LoadedModule { std::string file; uint64_t id; Image image; };
struct LoadSet { std::vector<LoadedModule> modules; };
LoadSet map_load_set(const std::filesystem::path& game_dir, uintptr_t base = default_base);
}
```

`map_prx` reads `sce_module/<name>`, checks it against `prx_sha256` in `loader/self.hpp` (a copy of the table in `docs/clean-room.md`, checked by `tools/common/test_pinned_hashes.py`) and maps it like `map_eboot`. Only the two PRXs of the load set have an entry there. `map_load_set` maps the eboot at `base`, then each PRX at the first address after the previous image's span that is aligned to its largest `p_align` (0x4000 in both), with module ids 1, 2, 3 in that order. It relocates nothing. `exports` checks that every export lies inside a segment of its image.

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

Supported: `R_X86_64_RELATIVE` (base + addend), `R_X86_64_64` (symbol + addend), `R_X86_64_GLOB_DAT` and `R_X86_64_JUMP_SLOT` (symbol), and `R_X86_64_DTPMOD64` (the `module_id` parameter, the TLS module id of the image being relocated: eboot 1, then the PRXs in loading order). DTPMOD64 ignores its symbol: in the PRXs every one names symbol 1, a local section symbol with an empty name and `st_shndx` 0, so the module is always the image itself. The eboot has none, and passes `eboot_module_id`. Any other type, `R_X86_64_DTPOFF64` and the other TLS types included, is an error that names the type and the offset.

A relocation against a defined symbol of the same file (`st_shndx != 0`) resolves to base + `st_value` without calling the resolver. The eboot has none, but the PRXs (M3) and synthetic test ELFs do. Undefined symbols with a `NID#library#module` name are imports and go to the resolver. The weak, untyped, undefined `module_start` and `module_stop` of the PRXs (`docs/formats/elf-dynamic.md`) are not imports: relocations against them resolve to 0, as for any unresolved weak symbol. Any other plain-named undefined symbol is an error.

Every import must resolve to a non-zero address. The game runtime, `bb-load` and the harness pass `bb::hle::resolve`.

M3 registers the PRXs' exports with `bb::hle` before relocating, so `resolve` binds imports to them first ([m3-core-os.md](m3-core-os.md#loading-the-bundled-prxs)).

`relocate` (declared in `loader/relocate.hpp`) applies `DT_SCE_RELA` first, then `DT_SCE_JMPREL`. It checks the type, then the target, then resolves the symbol, so the resolver never sees an import of a rejected relocation. Each 8-byte target must lie inside the memory of a `PT_LOAD`, and inside a writable one unless the image has `DT_TEXTREL` (checked without overflow). On an error the image is partly relocated and must be discarded. Calling `relocate` after `finalize_protection` is a `std::logic_error`. In the eboot, no relocation targets the code segment, 686 of the 701 imports are referenced, and `R_X86_64_64` against imported data uses only the addends 0 and 0x10.

## Imports and stubs

### Registry

```cpp
namespace bb::hle {
void register_function(std::string_view library, std::string_view nid, void* impl);
void register_export(std::string_view library, std::string_view nid, uintptr_t address);  // M3
void register_data(std::string_view library, std::string_view nid, DataInit init);   // M3, see Data imports
uintptr_t resolve(const loader::Import& imp);  // export, else impl, else stub
uintptr_t import_address(std::string_view library, std::string_view nid);  // cached result of resolve
enum class Binding : uint8_t { exported, implemented, stub, data_block };
std::optional<Binding> binding(std::string_view library, std::string_view nid);  // of resolve
void link(loader::LoadSet& set);  // hle/link.hpp: register the exports, relocate, register the TLS templates (M3)
StubObserver* set_stub_observer(StubObserver* observer); // returns the previous one;
                                                        // nullptr restores logging
Counts counts();                                // stubs and data blocks created so far
}
#define BB_HLE_FUNCTION(library, nid, function) …  // static registration
#define BB_HLE_DATA(library, nid, init) …          // the same for a data initializer
```

Implementations register by library name and NID, through `BB_HLE_FUNCTION` at namespace scope in each module's source file. Registering an import twice, or after it was resolved, is a `std::logic_error`. The exports of the loaded PRXs (M3) register the same way through `register_export`, under the same rules, and win over an implementation, which is then logged once. `resolve` returns the registered export, else the registered implementation, else a stub; `binding` says which, or nothing for an import never resolved. `link` registers every export of a load set (libc.prx's a second time under `libSceLibcInternal`, unless a module exports that NID there itself) and then relocates each module with `resolve` and its module id ([m3-core-os.md](m3-core-os.md#loading-the-bundled-prxs)). `resolve` is idempotent: each (library, NID) gets exactly one stub thunk or data block, created on the first call and cached. The eboot has thousands of relocations against a few hundred imports, and function-pointer equality and the per-import call counters depend on getting the same address every time.

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

The thunks live in anonymous pages of `bbhle`. A thunk is written while its page is `rw-`, and the page goes back to `r-x` right after. That is safe as long as `resolve` runs before game code; resolving while game threads may execute an earlier thunk of the same page (lazy PRX loading in M3) needs another scheme, for example a second, writable mapping of the same pages. Thunks, `StubInfo` objects and data blocks are never freed. In debug builds the same pages also hold the trace thunks of HLE implementations ([m3-core-os.md](m3-core-os.md#logging), "HLE call trace"), which use the same 24 bytes with another entry and are never freed either.

The NID name table is generated at build time from `symbols/nids.csv` by a small Python script into a sorted array that is searched with binary search. CMake reruns the script when the CSV changes. `bb::hle::nid_name(nid)` returns the name, or `?` for a NID the table doesn't have.

### Data imports

Each unresolved data import (`STT_OBJECT`) gets its own zero-filled 4 KiB block, also cached per (library, NID). An HLE module that has to give a data import a value registers an initializer for it (`bb::hle::register_data(library, nid, init)`, statically through `BB_HLE_DATA`, the same rules as for `register_function`). `resolve` calls it on the zero-filled block right after creating it, so before any code that imports it runs, and the block stays an ordinary data block (binding `data_block`, listed by `data_blocks()`), which the harness snapshots like the others. M3 uses this for the stack guard and `__progname` ([m3-core-os.md](m3-core-os.md#process)). An export of the same import wins, as for functions. `R_X86_64_64` relocations against imported data use small offsets (`symbol + 0x10`, see `docs/formats/elf-dynamic.md`), which stay inside the block. The harness compares these blocks like the data segment. It finds them through `bb::hle::data_blocks()`, a copy of the list of `DataBlock{library, nid, data}` in creation order, each `bb::hle::data_block_size` (4096) bytes.

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

**Size check.** `apply` asks `Image::function_size(link)`, which looks the address up in the binary search table of `PT_GNU_EH_FRAME` (`docs/formats/eh-frame.md`, dead FDEs excluded) and returns the FDE's range length only when an FDE starts exactly at that address. `apply` refuses an address that is not an FDE start and a function shorter than 14 bytes. `symbols/functions.csv` has no size column, so the FDE is the only source.

The table is untrusted input, checked in two places:
- **At map time** (`read_unwind_table`, before the range is reserved): at most one `PT_GNU_EH_FRAME`, inside the file data of a readable `PT_LOAD`; the header encodings of every pinned file (version 1, `eh_frame_ptr` pcrel|sdata4, `fde_count` udata4, table datarel|sdata4); the table fits `p_filesz` and its starts ascend strictly. Anything else is a `LoadError`, so an image with an unreadable table never maps. An image without `PT_GNU_EH_FRAME` maps, and `function_size` returns nothing for it.
- **At lookup** (`fde_range`): a binary search over the table. No entry that starts exactly at the address gives `nullopt`. An entry whose FDE or CIE is malformed, lies outside that segment's file data, uses an augmentation other than `z` + `R`/`L`/`P`, an FDE pointer encoding other than pcrel with a 4- or 8-byte format, or whose decoded `pc_begin` differs from the table's start, throws `LoadError`: the table points at broken data, which is not the same as "no function here".

**Protection.** In the runtime, hooks are applied before `finalize_protection`, while the code is still writable. When the image is already finalized (tests, `remove`), `apply` and `remove` make the affected pages (`page_down(entry)` to `page_up(entry + 14)`) writable themselves and give them their segment's final protection (`r-x`) back afterwards.

**Checks and saved bytes.** `apply` and `remove` check every entry of the span before they write anything, and throw `bb::hook::HookError` naming the replacement, the link address and the reason; a refused span patches nothing. Besides the size check, `apply` refuses a null `impl`, a patch that doesn't lie inside an executable segment, and a patch whose 14 bytes overlap those of another entry in the span or of a live hook (double hook). FDE ranges may overlap in untrusted unwind data, so a distinct FDE start alone doesn't rule this out. `remove` refuses an entry that isn't hooked; a refused span restores nothing.

The original 14 bytes of each patched entry live in a registry in `bbhook`, keyed by (image base, link address), next to the patch that was written. `Image` stays free of hook state, and the loader doesn't depend on `bbhook`. An image that is destroyed with hooks still applied leaves its entries behind, and a later image may be mapped at the same base. So an entry counts as hooked only while its patch is still in memory; one whose patch is gone is dropped. `apply` and `remove` are not thread-safe: they run while no other thread executes game code.

**Registration.** `BB_REPLACE(0x123450, my_function);` at namespace scope defines a static `bb::hook::Registrar` in an anonymous namespace, whose constructor calls `register_replacement` with the link address, `&my_function` and the name `"my_function"`. The registry behind `replacements()` is a function-local static vector in `bbhook`, in registration order; it checks nothing, `apply` does. `bbhook` is a `STATIC` library because it owns the registry but registers nothing itself ("Static registration").

**Calling the game.** A replacement calls a game function that isn't replaced yet through its link address, using the image accessor in `bbgame`:

```cpp
namespace bb::game {                  // src/game/image.hpp
loader::Image& image();               // the loaded eboot; aborts if none is set
void set_image(loader::Image* image); // called by the game runtime and the harness after loading
template <class F> F* fn(uint64_t link);  // image().fn<F>(link)
}
// in a replacement:
bb::game::fn<void(Foo*)>(0x123450)(foo);
auto* flags = reinterpret_cast<uint32_t*>(bb::game::image().runtime(0x5100000));
```

`image()` without an image prints `game: no image is loaded (bb::game::set_image was not called)` and aborts: a replacement that runs before loading is a bug, not an error to handle. `set_image(nullptr)` unsets it. `fn` throws `std::out_of_range` unless the address lies inside an executable segment, like `Image::fn`. `bbgame` is an `OBJECT` library that links `bbhook` and only the compile-time requirements of `bbhle`; executables link `bbgame` and `bbhle` themselves.

The game code and our code use the same System V calling convention, so a call needs no glue as long as no TLS is involved. From M3 the game uses `%fs` for its own TLS while our code uses glibc's, so transitions between the two would need glue. [m3-core-os.md](m3-core-os.md#tls-one-thread-pointer-for-game-and-runtime) avoids that: game and runtime share glibc's thread pointer.

**Calling imports.** A replacement calls an imported function through `bb::hle::import_address(library, nid)`, which returns the cached address that `resolve` handed out for that import (the same address the loader wrote into the image's GOT slot) and aborts if the import was never resolved. It never calls an HLE C++ function directly. Then the original and the replacement reach the same stub or implementation, and the stub logs compare.

## Differential test harness

### Process setup

One GoogleTest fixture, `bb::test::EbootTest` (`tests/harness/eboot.hpp`), loads the eboot once per test binary, in the `SetUp` of the first test that uses it: `map_eboot` at the default base, `relocate` with `bb::hle::resolve`, `finalize_protection`, and `bb::game::set_image`. The image and one harness on it (`image()`, `harness()`) then live in a function-local static until the process exits, and every `SetUp` restores the baseline. Without the game, `SetUp` skips the test as `BB_REQUIRE_GAME` does. Because the fixture's harness lives as long as the process and only one harness may exist at a time, tests that use `EbootTest` go into binaries of their own, apart from the synthetic-ELF tests with their per-test harness: `harness_eboot_test` for the harness itself and `game_eboot_test` (`tests/game/`) for the decompiled functions in `bbgame`, both with the same `main.cpp` and the fixture from the static library `bbeboottest`. ctest runs each of them as one test (`add_test`, not `gtest_discover_tests`, which would start a process per test and load the eboot each time). Imports with a registered HLE implementation call it; all others go to stubs, and the harness installs a stub observer that records those calls. **Hooks are never applied in the harness.** It calls the original at its runtime address and the replacement as a C++ function.

No game initialization code runs: not `DT_INIT`, `DT_PREINIT_ARRAY`, `DT_INIT_ARRAY` nor the entry point. The baseline is the static image right after relocation. A function that depends on state set up by init code needs that state built by its test's input builder.

**With the load set (M3).** A function that allocates needs libc's `malloc` family, so libc.prx loaded and initialised, and with it the game's allocator. A second fixture, `bb::test::LoadSetTest` (`tests/harness/load_set.hpp`, also in `bbeboottest`), does that once per test binary, on the thread of its first test: `map_load_set` at the default base, `bb::hle::link`, `finalize_protection` for every module, `bb::game::set_image` with the eboot, `bb::hle::run_prx_init`, then `free(malloc(1))` through libc.prx's exports, which maps the game's 116 MiB heap as direct memory, and the thread's TLS blocks of both PRXs through the TLS helper (`tls_block_address` with offset 0). Only then does it construct the harness on the eboot, with these extra regions: the PRXs' writable segments (named after the file, reported as link addresses), the TLS blocks (`tls libSceFios2.prx`, `tls libc.prx`), the thread's eboot TLS block (`tls eboot.bin`), its guest errno (`guest errno`) and every readable and writable guest mapping (`mapping <address> <name>`), direct memory among them as a sparse region of the direct memory memfd. Everything that the first allocation or TLS access would otherwise create after the baseline is in it, so a restore never takes back the allocator's state while its heap stays mapped. The eboot's own init still doesn't run. A replacement allocates as the original does, through the import (`bb::hle::import_address("libc", "gQX+4GDQjpM")`), so both reach the same allocator. The TLS regions are that thread's, so `SetUp` fails a test on any other thread. The load set and the harness are never destroyed: the game's memory holds pointers to runtime objects (the host mutexes behind libc's guest mutexes), so the writable segments and the mappings are LeakSanitizer root regions (`bb::test::scan_for_leaks` in `tests/support/leaks.hpp`, `__lsan_register_root_region`) and stay mapped until exit. The HLE registry takes one load set per process and only one harness may exist, so these tests have a binary of their own, `harness_load_set_test`; decomp tests of functions that allocate will need one too. In the debug preset the setup takes about 1 s and a compare of 0x3cc720 (one allocation) about 6 ms. With the heap as a dense region a compare took about 40 ms and the process about 230 MiB more.

After loading, the harness takes a **baseline snapshot** of all writable memory it owns:
- the image's writable segments (data and `.bss`, about 6 MiB),
- the data-import blocks,
- the regions its caller passes (`extra`), for state outside the image: with the load set loaded, the PRXs' writable segments, the game's heap and the test thread's TLS ([m3-core-os.md](m3-core-os.md#memory), "Harness"),
- the **arena**, an mmap'd region of 16 MiB that the harness owns for test inputs,
- optionally a `State` for what isn't memory it can copy: with the load set loaded, the guest mappings and direct memory's allocations ("Guest memory" below).

A region is either **dense**, every byte copied and compared, or **sparse**: a shared mapping of a sparse file, such as direct memory (the 116 MiB heap of the game's allocator is a view of the direct memory memfd). For a sparse region the harness keeps only the pages that are data in the file, found with `lseek(SEEK_DATA/SEEK_HOLE)`, since a hole reads zero. The restore punches a hole (`fallocate(FALLOC_FL_PUNCH_HOLE)`) into every data page the baseline didn't have and copies the baseline's pages back; the comparison looks only at the pages that are data after either call, and reads a page that is a hole as zeros rather than through the mapping, which would allocate it. So a sparse region costs time and memory in proportion to the pages a test touches, not to its size. Unlike page residency (`mincore`), the file's data/hole map doesn't depend on whether a page is in memory: tmpfs reports a swapped-out page as data. A sparse region's address, file offset and size must be multiples of the host page size (the constructor throws `std::invalid_argument` otherwise).

### Harness and arena

`src/harness/harness.hpp` (#46):

```cpp
namespace bb::harness {
class Arena {                        // 16 MiB, mmap'd, zero in the baseline
    void* alloc(std::size_t n, std::size_t align = 16);  // zeroed
    template <class T> T* zeroed(std::size_t count = 1);  // trivial T only
    char* str(std::string_view s);   // with a terminating NUL
    std::byte* bytes(std::span<const std::byte> b);
};
struct StubCall {
    const hle::StubInfo* stub;
    std::array<std::uint64_t, 6> args;  // rdi, rsi, rdx, rcx, r8, r9
};
struct Region {
    std::string name;                // in the report
    std::byte* data;
    std::size_t size;
    std::uint64_t link;              // link address of data for an image segment, else 0
    bool is_image;                   // reported as link addresses, else as offsets
    int fd = -1;                     // sparse: the file that data maps, and data's offset in it
    std::uint64_t fd_offset = 0;
};
class State {                        // state outside the regions, with its own baseline
    virtual std::string_view name() const = 0;          // in the report
    virtual void restore() = 0;                         // baseline back, before the regions
    virtual std::vector<std::string> changes() const = 0;  // since the baseline, compared
    virtual bool intact() const { return true; }        // false: baseline gone, regions unread
};
class Harness : private hle::StubObserver {  // installs itself as the stub observer
    explicit Harness(loader::Image& image, std::vector<Region> extra = {},
                     State* state = nullptr);  // takes the baseline
    Arena& arena();
    void restore();                  // baseline back into the state and every region, arena
                                     // allocation at 0, empty stub log
    const std::vector<StubCall>& log() const;  // stub calls since the last restore
    void declare_arity(std::string_view library, std::string_view nid, int n);  // 0..6
    template <class Sig, class Build>
    Result compare(std::uint64_t link_addr, Sig* replacement, Build&& build);
    template <class Sig, class Build>  // a synthetic original of this process, for harness tests
    Result compare_functions(Sig* original, Sig* replacement, Build&& build);
};
class Result {                       // true when nothing differs
    const std::string& report() const;  // one line per difference, empty when true
};
}
```

The arena is a bump allocator that starts at offset 0 again at every restore, so a builder that makes the same calls gets the same addresses in both runs, and the layout is the same in every run of a build. `alloc` throws `std::length_error` when the arena is full. The builder returns a `std::tuple` that converts to the parameter types of `Sig`. Floating-point return values are compared bitwise, so NaN payloads and the sign of zero count, and a `void` signature compares only memory and the stub log.

A report line names each difference: `return value: original 0x28, replacement 0x29`, or `memory: image 0x4100..0x4101 differs: original 15, replacement 16` (link addresses, end exclusive, up to 8 bytes of each side shown) and `memory: arena +0x4..+0x5 differs: …` (offsets). At most 8 ranges are listed, then their total. A data block is the region `data block <library> <nid>`, reported as offsets like the arena. Only the blocks that exist when the harness is constructed are in the snapshot. A stub log difference gives `stub log: differs at call 2 (original 2 calls, replacement 2)` (counted from 1), then up to 16 calls of each side as `  original 1: libTest harnessImpA (?) 0x… 0x… 0x… 0x… 0x… 0x…` (library, NID, name, all six arguments). A state difference gives `guest memory: differs at change 1 (original 1 changes, replacement 1)`, then up to 16 changes of each side as `  original 1: <line>`. `declare_arity` throws `std::invalid_argument` outside 0..6. Only one harness may exist at a time, because it is the process's stub observer from construction to destruction. The constructor throws `std::logic_error` if a stub observer is already installed, so a second harness cannot silently take the log away from the first. The harness core needs only a mapped and relocated `Image`, so its tests use a synthetic ELF with hand-assembled functions and run without the game.

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

   - the state's changes, line by line.

Not compared: the stack below the call, registers other than the return registers, the code segment.

### Guest memory

A map call during a test (#206) changes state that no region holds: the kernel's region table, a new host mapping in the guest window and, for direct memory, the allocations and the memfd pages behind them. `LoadSetTest` gives its harness a `State` for it (`GuestMemory` in `tests/harness/load_set.cpp`), which takes its baseline together with the harness's, after the heap exists. The kernel side is a small API for the harness only:

- `vmem::regions()` is the snapshot of the region table. `vmem::restore(baseline)` gives back every region that isn't in the baseline as `unmap` does (the guest window's part is reserved again), so the next map without a fixed address takes the same lowest free range again ([m3-core-os.md](m3-core-os.md#memory), "Guest window"). It can't map a region of the baseline back, so if one is gone or changed (unmapped, reprotected, split) it changes nothing and returns `EINVAL`; the fixture's `restore` then throws `std::runtime_error`. A test must leave the baseline's mappings alone, which also keeps them readable for the snapshot. `intact()` checks that every baseline mapping is still there unchanged: if the original's call broke it, the harness throws `std::runtime_error`; if the replacement's did, the comparison fails with `memory: not compared` and reads no region (one may now be `PROT_NONE`), so the test that caused it fails, though the next restore still throws.
- `dmem::allocations()` is the snapshot of the allocations. `dmem::restore_allocations(baseline)` puts them back and punches a hole into the memfd wherever an allocation lies now that the baseline doesn't cover, so the next allocation gets the same offset and reads zeros. The data of the baseline's allocations stays the harness's to restore: the heap is a sparse region. Data of a baseline allocation that no region maps isn't restored.

`changes()` lists the baseline's mappings that are gone or changed (`mapping <start>..<end> prot <p> flags <f> offset <o>`, plus `reserved` for a reservation, then `removed`), the mappings that aren't in the baseline (the same without `removed`; a changed mapping appears in both), then the allocations made (`direct memory allocated <start>..<end> type <t>`) and released (`… released …`), each in address order. Since the placement is deterministic, the original's and the replacement's mappings compare by address, length, protection, kind and direct memory offset. Not compared: the contents of mappings made during the call (a test reads them through the return value if it needs them) and names. Map calls go to HLE implementations, so they aren't in the stub log.

No eboot function that maps memory is a small leaf: the map calls (0x2081dbe, 0x33eb0d, 0x26d3fda) sit in allocator methods that lock and fill their own tables. The tests in `tests/harness/load_set_test.cpp` therefore use synthetic guest-ABI originals compiled into the test, which call the eboot's imports as those call sites do, through `compare_functions`. A version that maps a different size or protection, or allocates a different size, fails; two runs of a map get the same address and read zeros.

### Limits for M2

- No TLS: functions that use `%fs` must not be called until M3 sets up TLS. The harness doesn't set `%fs`, so such a function would read and possibly overwrite glibc's own TLS block and thread control block, which can crash or corrupt the test process. From M3 the eboot test executables (`harness_eboot_test`, `game_eboot_test`) link the eboot's TLS reservation (`bbeboottls`), so `%fs` accesses land in it. `EbootTest` doesn't snapshot it; `LoadSetTest` does.
- HLE implementations called from a diff test must be stateless, or keep their state in a snapshot region. Other state isn't restored between the two calls, and calls to implementations aren't compared.
- No heap with `EbootTest`: an import such as `malloc` is a stub that returns 0. `LoadSetTest` runs libc.prx's `malloc`, the game's allocator, on a heap that is a snapshot region. A map call during a test (the heap growing, or a function that maps memory itself) is taken back by the restore and compared ("Guest memory").
- A crash in the original or the replacement kills the test binary. Isolating each call in a forked child can come later if it is needed.
- The original code isn't instrumented by AddressSanitizer. Only the replacement and the harness are.
- AddressSanitizer's stack-use-after-return detection, on by default, moves an instrumented function's locals into fake stack frames on the heap. Pointers to the replacement's locals would then not look like stack pointers and would compare unequal. The harness turns it off with `__asan_default_options()` returning `detect_stack_use_after_return=0`, defined in the harness tests' own main (`tests/harness/main.cpp`, instead of `gtest_main`) so the linker keeps it.
- With clang, `-fsanitize=undefined` includes `-fsanitize=function`, which checks the type of indirect calls. The game code and the stub thunks were not compiled with this check, so clang builds turn it off with `-fno-sanitize=function`. No clang build has been tested yet (CI and local builds use GCC). GCC's `-fsanitize=undefined` has no such check.

## Tools

`bb-load` maps the load set, binds and relocates it through `bb::hle::link`, finalizes the protections and exits without running game code. For each module (file and module id) it prints the base, the entry point, the segments (link address, memory and file size, protection), the TLS template, the `PT_SCE_PROCPARAM` address, the number of relocations by type over both tables, and the imports by library (functions and objects, in the order of `tools/elfimports`, and how many of them `resolve` bound to a bundled export, an HLE implementation or a stub or data block, or never resolved because nothing references them). Last come the numbers of function stubs and data blocks that `resolve` created over the whole load set. The test checks the 3 data blocks exactly and the number of stubs only against an upper bound, so that a new HLE function doesn't have to edit it. It links `bbhlenet`, so the network imports that module implements count as HLE, not as stubs.

The game directory comes from `--game-dir` or else `BB_GAME_DIR` in the environment. Unlike the tests, `bb-load` doesn't read `local.env`: it is a user-facing program and runs outside the repository too. A test runs it on the game and compares the eboot's output with fixed expected values, and the bases, DTPMOD64 counts and bindings of the PRXs. The imports by library are those of `tools/elfimports` for `eboot.elf` (701 imports in 39 libraries), copied into the test.

`bb-run` runs the game (options in `src/tools/run_options.hpp`). The game directory comes from `--game-dir`, else `BB_GAME_DIR` in the environment, else `BB_GAME_DIR` in `./local.env`. Start-up, on the initial thread: the log filter from `--log` or `BB_LOG` (before the link, so that the HLE call trace applies), `RuntimeDirs` for the profile, the log file (unless `--no-log-file`), the load set linked through `bb::hle::link`, every registered replacement applied to the eboot, the final protections, `bb::game::set_image`, the TLS check, the game mounted at `/app0`, the save data root of the profile and the profile name as the user name. Then the game's main thread (16 MiB plus the thread stack reserve) runs the PRX init and calls the entry with `argv[0] = "/app0/eboot.bin"` and an exit handler that only tells the main-loop probe that `main` has returned. The initial thread waits for it. With `--timeout` it logs `timeout: the game still runs after <n> s` at that point and exits with status 124, as timeout(1). Every exit after the load flushes the log and calls `_exit`, never `exit`. `bb-run` links `bbhle`, `bbhlenet`, `bbhlesystem`, `bbhlevideo`, `bbeboottls` and `bbgame`. The main-loop probe (`src/tools/main_loop_probe.hpp`, see m3-core-os.md, "Goal") is the libScePosix `gettimeofday` observer. When it fires, on the game's main thread, `bb-run` logs `main loop reached after <n> s` (seconds since the start of `bb-run`, one decimal) and, with `--stop-at-main-loop`, exits with status 0 through the same path. A game test runs it for 10 s with the XDG directories in a temporary directory and checks the PRX init, the entry call, the eboot's first HLE call, the exit status and the log file. A second one runs `--stop-at-main-loop --timeout 120` and expects status 0 and the log line; it is disabled until the game gets there (#106).

`bb-gcn disasm` disassembles a GCN shader binary (see [graphics.md](graphics.md#interface), "Disassembler").

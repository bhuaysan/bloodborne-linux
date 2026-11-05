# Graphics design (M4)

How the game's GPU work reaches Vulkan: the shader translator (GCN → SPIR-V), how it is tested offline, how shaders are extracted from the game files, and a sketch of the command-buffer side. The first wave of M4 issues builds on this note and runs without the game booting. Change this note in the same PR when an implementation has to deviate.

## Starting point

- **GPU generation.** The base PS4 GPU is GCN 2nd generation ("Sea Islands", gfx7). The game's shader code confirms the SI/CI encodings: exports are encoded as `0xF8……` (gfx8 uses `0xC4……`), interpolation as `0xC8……` (gfx8: `0xD4……`), and `v_cvt_pkrtz_f16_f32` appears as a VOP2 instruction, which gfx8 only has as VOP3. Whether any gfx7-only instruction (FLAT, the CI additions) occurs is still open. References: the AMD documents in [clean-room.md](../clean-room.md#allowed-with-credit).
- **Command buffers** are built by GNM code that is statically linked into the eboot. Only the driver boundary is imported: `libSceGnmDriver` has 50 imported functions, two of them named so far. The game hands us finished PM4 streams at submission time.
- **Shaders live in the game files** (below). Each one can be found again at run time by the hash of its code.
- **M3 doesn't reach the main loop yet.** The first wave therefore works on the game files only: extraction, decoding, translation and execution tests on our own GCN programs.

## Source layout

```
src/gpu/
  gcn/     bbgcn    decoder, disassembler, analysis, translator, SPIR-V builder (no Vulkan, no loader)
  pm4/     bbpm4    PM4 parser and GPU register state (later wave)
  vk/      bbvk     Vulkan backend: device, memory, pipelines, presentation (later wave)
src/tools/bb-gcn    command line: disassemble or translate one shader binary
tools/shaderbnd/    Python: extracts the shaders from the game's archives into analysis/
tests/gpu/          unit, corpus and execution tests
```

`bbgcn` depends only on `bbcommon`, so it builds and tests without Vulkan. `bbvk` depends on `bbgcn` and `bbpm4`. The HLE side (`libSceGnmDriver`, `libSceVideoOut`) depends on `bbvk` and lives in `src/hle/` like the other modules. Namespaces: `bb::gcn`, `bb::pm4`, `bb::vk`.

Build requirements: Vulkan headers and loader (installed), SPIR-V headers (`spirv-headers-devel`) for the enum values. Test tools, run as programs: `llvm-mc` and `llvm-objdump` from the `llvm` package (GCN assembler and disassembler for gfx7), `spirv-val` from `spirv-tools`, and lavapipe (Mesa's CPU Vulkan driver, installed with `mesa-vulkan-drivers`). Tests that need a missing tool skip with a message, like tests that need the game.

## Shader extraction

The 8 archives in `dvdroot_ps4/shader/` hold about 20,300 shader programs:

| Archive | Files | Programs |
|---|---|---|
| `gxflvershader.shaderbnd.dcx` | 14571 | 16265 |
| `gxflvershader.shaderbnddebug.dcx` | 3194 | 3194 |
| `gxffxshader.shaderbnd.dcx` | 301 | 308 |
| `gxdecal.shaderbnd.dcx` | 193 | 193 |
| `gxposteffect.shaderbnd.dcx` | 119 | 119 |
| `gxrenderershader.shaderbnd.dcx` | 111 | 111 |
| `gxshader.shaderbnd.dcx` | 15 | 15 |
| `gxgui.shaderbnd.dcx` | 13 | 13 |

Programs are counted by their `OrbShdr` marker, which matches the count of `s_endpgm` in every archive.

What our first look shows (to be worked out and documented in `docs/formats/`):

- **DCX**: a big-endian header with the sizes (`DCS` chunk) and a `DFLT` method tag, followed by a zlib stream. `docs/formats/dcx.md`.
- **BND4**: an archive with a file table, names and data offsets. No nested archives in the shader set. `docs/formats/bnd4.md`.
- **Shader program**: GCN code first, ending in `s_endpgm`, then a footer that starts with the `OrbShdr` marker. The footer holds the metadata: resource names (`g_Texture0`, `SS_ClampLinear`), input semantics (`IN.tex0`, `TEXCOORD`) and the entry point name. Some files hold more than one program. `docs/formats/shader-binary.md`.

Material on these formats from category 3 of the code sources may be used under its rules. Everything gets checked against the files.

`tools/shaderbnd` checks each archive's SHA-256 against a pinned table in `tools/common/` and refuses other files. It writes to `analysis/shaders/<archive>/`:

- one file per program: code plus footer, as stored;
- `index.json` with entry name, program index, stage, code size and the SHA-256 of the code (the **code hash**).

Nothing it writes is committed. The code hash is the shader's identity everywhere: corpus reports, the translation cache, and later at run time, where the game gives us only the code address (`SPI_SHADER_PGM_LO_*`). Whether the footer follows the code in GPU memory, so the run time can find the code size the same way, is checked once the game boots. Until then the run time decodes up to `s_endpgm`.

The eboot may contain shaders of its own (GNM's internal clear and copy shaders, fetch shaders). The extractor also scans the eboot's data for the `OrbShdr` marker and reports what it finds.

## Shader translator

```
code ─decode─► instructions ─cfg─► blocks ─analyze─► typed program ─structurize─► SPIR-V
                                                 ▲
                                       ShaderKey (pipeline state)
```

### Decoder and disassembler

- Table-driven decoding of every Sea Islands microcode format: SOP2, SOPK, SOP1, SOPC, SOPP, SMRD, VOP2, VOP1, VOPC, VOP3, VINTRP, DS, MUBUF, MTBUF, MIMG, EXP, FLAT. Literal constants and inline constants are handled.
- The result is a list of `Instruction` values (opcode, format, operands, modifiers, size), stopping after `s_endpgm`.
- The disassembler prints LLVM's assembly syntax, so its output can be compared line by line with `llvm-objdump` on the whole corpus.
- An unknown encoding is an error that names the offset and the word. It is never skipped.

### Execution model

**One SPIR-V invocation is one GCN lane.** The registers of the wave become per-invocation variables:

- **VGPRs** are the lane's own values.
- **SGPRs** are copied into every lane. They are uniform in the hardware, and every lane computing the same scalar value gives the same result.
- **Lane masks** (EXEC, VCC, and SGPR pairs that hold compare results or saved masks) are stored per lane as **this lane's bit**: a bool. The bitwise operations on masks (`s_and_b64`, `s_or_b64`, `s_andn2_b64`, `s_and_saveexec_b64`, …) then become boolean operations on that bit. A type pass over the program decides which SGPRs hold masks and which hold values. An SGPR used as both is reported as an error until a shader needs it.
- **Cross-lane operations** need the whole mask: SCC after a mask operation, `s_cbranch_execz` and `s_cbranch_vccz`, `s_bcnt1`/`s_ff1` on masks, `v_readfirstlane_b32`, and DS swizzle and permute operations. They use subgroup operations (ballot, any, broadcast first).

**Divergence is predication.** A GCN program's control flow is scalar, so the whole wave takes every branch together. Lanes diverge only through EXEC. The translator mirrors that:

- The branch structure comes from the scalar branches, which are uniform, and becomes structured SPIR-V control flow. The structurizer handles acyclic regions and natural loops. An irreducible graph falls back to a loop around a switch on a block index.
- A VALU result is written as `select(active, new, old)`. The operation itself runs in every lane. This keeps implicit derivatives in pixel shaders defined, because image sampling is never placed under non-uniform control flow.
- Side effects (memory stores, atomics, exports, LDS writes) run under `if (active)`.

**Wave size.** A GCN wave has 64 lanes, but a Vulkan subgroup may be smaller (32 on many GPUs, less on lavapipe). Two modes:

- **Exact:** the device can require a subgroup size of 64 (`VK_EXT_subgroup_size_control`). AMD RDNA GPUs can.
- **Lane-local:** cross-lane results are computed over the subgroup instead of the wave. That is exact for uniform values and for "is any lane active" skips. It differs only when a value really depends on lanes in another subgroup.

An analysis pass marks the shaders whose results can depend on the wave width, so the corpus report shows how many are affected.

### Resources and memory

GCN shaders get their resources as descriptors in SGPRs:

| Descriptor | Meaning |
|---|---|
| V# | buffer |
| T# | image |
| S# | sampler |

The descriptors come either directly from user-data registers or through `s_load_*` from a pointer that itself comes from user data. A dataflow pass records the **source** of every descriptor that a memory instruction uses: user-data slot *k*, or a load at offset *o* through the pointer in slot *k*, nested as deep as needed. The list of sources is part of the shader's interface.

- **Buffers and scalar memory** are read and written through buffer device addresses (`VK_KHR_buffer_device_address`, `PhysicalStorageBuffer` in SPIR-V). The base address comes from the V# at run time, as data. A per-draw uniform maps guest GPU addresses to device addresses. So buffers need no static bindings. Typed buffer formats (MTBUF, or the V# format) are converted in the shader from raw dwords.
- **Vertex fetch** is ordinary buffer loading. PS4 vertex shaders load their attributes themselves, through a fetch subroutine called with `s_swappc_b64`. The translator inlines the fetch subroutine, which it receives as a second code input. Vulkan vertex input state stays empty. Where the fetch code comes from (eboot, generated at run time, or the shader archives) is part of the GNM survey issue.
- **Images and samplers** need bindings. Each T# and S# source gets an index into descriptor arrays. The backend reads the descriptor contents at draw time and supplies matching views and samplers.
- **LDS** becomes a `Workgroup` array. **GDS and ordered append** are reported as unsupported until a shader needs them.

### Stage interfaces and ShaderKey

The register state a program starts with depends on pipeline registers. That covers which SGPRs hold user data and which VGPRs hold the vertex index, the instance ID, barycentrics or the position. So do the stage's inputs and outputs:

| Stage | Inputs | Outputs |
|---|---|---|
| VS | vertex and instance index in VGPRs (`SPI_SHADER_PGM_RSRC*`, `VGT_*`) | `exp pos0` → `Position`, `exp param<n>` → `Location n` |
| PS | `v_interp_*` attribute *n* → location through `SPI_PS_INPUT_CNTL_n`; `SPI_PS_INPUT_ENA/ADDR` decide which VGPRs are set | `exp mrt<n>` → output *n*, converted per `SPI_SHADER_COL_FORMAT`; `exp mrtz` → `FragDepth` |
| CS | thread and group IDs per `COMPUTE_PGM_RSRC2` and `COMPUTE_NUM_THREAD_*` | memory only |

`ShaderKey` holds the values of these registers that the translation depends on, and nothing else. Same code hash plus same key gives the same SPIR-V. The translation cache is keyed by both and lives in `$XDG_CACHE_HOME/bloodborne-linux/shaders/`. Offline, keys come from the footer metadata where it carries them, otherwise from test defaults.

Other stages (LS, HS, ES, GS for tessellation and geometry shaders) wait until the corpus shows that the game uses them.

### SPIR-V builder

`bbgcn` has its own small module builder that emits the word stream directly. The enum values come from the Khronos SPIR-V headers. Every translated module must pass `spirv-val` in the tests. An optimizer (`spirv-opt`) can come later, behind the cache.

### Interface

```cpp
namespace bb::gcn {

enum class Stage { vertex, pixel, compute };

class GcnError : public std::runtime_error { /* offset and word of the failing instruction */ };

std::vector<Instruction> decode(std::span<const std::uint32_t> code);   // throws GcnError
std::string disassemble(const Instruction& inst);    // LLVM syntax, one line

struct ShaderKey { Stage stage; /* register values the translation depends on */ };

struct ShaderInterface {
    std::vector<ResourceSource> images, samplers;     // binding index = position
    std::vector<Attribute> inputs, outputs;
    bool wave_size_dependent;                         // result can differ in lane-local mode
};

struct Translation { std::vector<std::uint32_t> spirv; ShaderInterface interface; };
Translation translate(std::span<const std::uint32_t> code,     // throws GcnError
                      std::span<const std::uint32_t> fetch,
                      const ShaderKey& key);
}
```

Errors are exceptions, as in the loader and hooks (C++20, no `std::expected`). The exact members are settled by the decoder and translator issues, and recorded here when they are.

## Offline test strategy

Four levels. The first three need neither a running game nor a GPU.

1. **Unit tests** (CI):
   - the decoder, per microcode format, with words encoded by hand from the ISA manual;
   - the disassembler text;
   - the SPIR-V builder, through `spirv-val`.
2. **Corpus tests** (local, need the game; skip without `BB_GAME_DIR`). Over every program that `tools/shaderbnd` extracted:
   - it decodes completely, ends at `s_endpgm`, and its size matches the footer;
   - our disassembly equals `llvm-objdump --mcpu=bonaire` (gfx7) line by line, after normalizing whitespace;
   - it translates with the default key, and the module passes `spirv-val`;
   - a report counts opcodes, untranslated instructions and wave-size-dependent shaders. It goes to `analysis/`, not into the repo.
3. **Execution tests** (CI with lavapipe):
   - Small GCN programs are written in assembly by us, under `tests/gpu/programs/`. `llvm-mc` assembles them into binaries, which are committed next to the source so that CI doesn't need LLVM. A test checks that the binaries match the source when `llvm-mc` is installed.
   - Each program is translated and run as a compute shader on lavapipe, with input buffers. The output is compared with values we work out by hand from the ISA manual. This covers ALU semantics (output modifiers, clamping, denormals, 24-bit multiplies), EXEC predication and loops, scalar and buffer memory, and LDS.
   - One graphics test renders a VS and PS pair into an offscreen image.
   - Lavapipe's subgroups are smaller than 64, so these tests cover the lane-local mode. The exact mode runs locally on an AMD GPU.
4. **Replays** (later wave, after M3):
   - Captures of real frames go to `analysis/captures/`: PM4 streams plus the memory they reference. They come from our runtime, or as reference frames from an emulator run on the game with call logging off (see [clean-room.md](../clean-room.md#code-sources)).
   - The captures are replayed through `bbpm4` and `bbvk`, and the images are compared.

Committed test data is only our own: assembly sources, their assembled binaries, and expected values. Nothing extracted from the game is committed.

## Command buffers and state (later wave, sketch)

- **Submission.** The `libSceGnmDriver` submit functions hand `bbpm4` the command buffers and the constant-engine buffers. Parsing happens on a GPU thread, never on the calling game thread. Imports that write packets into a game buffer write standard PM4 (per the SI programming guide), so the parser sees one format.
- **Register state.** `SET_CONTEXT_REG`, `SET_SH_REG`, `SET_UCONFIG_REG` and `SET_CONFIG_REG` update a mirror of the register file.
- **Work packets.** Draw and dispatch packets take a snapshot of the state that the backend needs: ShaderKey, render targets, blend, depth and raster state, user data.
- **Unknown packets** are logged once per opcode. `NOP` packets with a payload are passed to a hook list, because the driver may use them as markers.
- **Register names and offsets** come from the AMD register guide. The Mesa register database (code sources category 1) may be used to generate a table, as generated data in its own PR.
- **Memory.** The guest's GPU-visible memory (libkernel direct memory, M3) is imported into Vulkan with `VK_EXT_external_memory_host` where the device allows it, giving device addresses for buffers without copies.
- **Textures and render targets** are cached by address and descriptor. Tiled ↔ linear conversion uses AMD's addrlib (code sources category 1, MIT), copied into `third_party/` in its own PR.
- **Flip.** `libSceVideoOut` flips present through a Vulkan swapchain.

## First wave (offline)

Each item is one issue, at most 500 lines of code and tests. Items 1–3 and 5 can start at once.

1. **DCX and BND4 formats and `tools/shaderbnd`.** Covers the format notes, pinned archive hashes and extraction into `analysis/shaders/`.
2. **Shader binary footer.** The format note, plus stage, code size and resource names in `index.json`. Includes the eboot scan for embedded programs. Depends on 1.
3. **`bbgcn` decoder and disassembler.** Covers all Sea Islands formats, with unit tests per format.
4. **Corpus decode test against LLVM.** Decode every program, compare with `llvm-objdump`, and answer the open SI/CI question. Depends on 2 and 3.
5. **SPIR-V builder and `spirv-val` test support.**
6. **Translator core.** CFG, mask typing, structurizer, predication, the SALU and VALU subset, the compute stage, and the lavapipe execution harness with `llvm-mc`-assembled test programs. Depends on 3 and 5.
7. **Memory instructions.** Descriptor source analysis, SMRD, MUBUF and MTBUF through device addresses, and LDS. Depends on 6.
8. **Graphics stages.** VS exports, PS interpolation and MRT exports, MIMG sampling with bindings, and the offscreen render test. Depends on 7.
9. **Corpus translation report.** Translate everything, run `spirv-val` and record coverage. Depends on 8. It can start as a partial report after 6.
10. **GNM survey (research).** How the eboot builds and submits command buffers, what the `libSceGnmDriver` imports do (behavior only, named by role), and where fetch shaders come from. Written up in `docs/formats/gnm.md`. Read-only, and needed for the second wave.

## Open questions

- Is the code gfx7 or gfx6? The corpus test answers this (item 4).
- Does the footer follow the code in GPU memory? This is checked at run time.
- Where do fetch shaders come from? Item 10 answers this.
- How often do shaders depend on the wave size? Item 9 counts this.
- Does the game use LS/HS/ES/GS stages? The footer stage field answers this (item 2).

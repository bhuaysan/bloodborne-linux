# Graphics design (M4)

How the game's GPU work reaches Vulkan: the shader translator (GCN → SPIR-V), how it is tested offline, how shaders are extracted from the game files, and a sketch of the command-buffer side. The first wave of M4 issues builds on this note and runs without the game booting. Change this note in the same PR when an implementation has to deviate.

## Starting point

- **GPU generation.** The base PS4 GPU is GCN 2nd generation ("Sea Islands", gfx7). The game's shader code uses the SI/CI encodings. Three instructions from a game shader show it:
  - `v_interp_p1_f32` (`0xC8080000`), `image_sample` and `exp mrt0 … done compr vm` (`0xF800…`), assembled with LLVM's assembler for `bonaire` (gfx7), give the same bytes as the game.
  - LLVM's gfx8 (`tonga`) decoder rejects them as invalid encodings.
  - `v_cvt_pkrtz_f16_f32` appears as a VOP2 instruction, which gfx8 only has as VOP3.
  
  Whether the corpus uses CI-only instructions (FLAT, the CI additions) is still open. References: the AMD documents in [clean-room.md](../clean-room.md#allowed-with-credit).
- **Command buffers** are built by GNM code that is statically linked into the eboot. Only the driver boundary is imported: `libSceGnmDriver` has 50 imported functions, two of them named so far. The game hands us finished PM4 streams at submission time.
- **Shaders live in the game files** (below). Each one can be found again at run time by the hash of its code.
- **M3 doesn't reach the main loop yet.** The first wave therefore works on the game files only: extraction, decoding, translation, and execution tests on our own GCN programs.

## Source layout

```
src/gpu/
  gcn/     bbgcn    decoder, disassembler, analysis, translator, SPIR-V builder (no Vulkan, no loader)
  pm4/     bbpm4    PM4 parser and GPU register state (later wave)
  vk/      bbvk     Vulkan backend: device, memory, pipelines, presentation (later wave)
src/tools/bb-gcn    command line: disassemble or translate one shader binary
tools/shaderbnd/    Python: extracts the shaders from the game's archives into analysis/
tests/gpu/gcn/      bbgcn unit, corpus and execution tests (one directory per target, as in runtime.md)
tests/gpu/runner/   bbcomputerunner: the test-only Vulkan compute runner on lavapipe (built only with Vulkan)
tests/gpu/programs/ our GCN test programs: assembly source plus assembled words as hex text
```

Dependencies:
- `bbgcn` depends only on `bbcommon`, so it builds and tests without Vulkan.
- `bbvk` depends on `bbgcn` and `bbpm4`.
- The HLE modules for `libSceGnmDriver` and `libSceVideoOut` (later wave) get their own `OBJECT` library on top of `bbvk`, so the harness test binaries that link `bbhle` don't pull in Vulkan.

Namespaces: `bb::gcn`, `bb::pm4`, `bb::vk`.

Build and test requirements:

| Need | Package (Fedora / Ubuntu) | Used for |
|---|---|---|
| SPIR-V headers | `spirv-headers-devel` / `spirv-headers` | enum values for the SPIR-V builder (build dependency of `bbgcn`) |
| Vulkan headers and loader | `vulkan-headers`, `vulkan-loader-devel` / `libvulkan-dev` | execution tests; the targets that need it are built only when `find_package(Vulkan)` finds it |
| lavapipe | `mesa-vulkan-drivers` | Mesa's CPU Vulkan driver, for execution tests |
| SPIR-V validator | `spirv-tools` | `spirv-val` |
| LLVM | `llvm` | `llvm-mc`, used as GCN assembler for gfx7 |

All of these are run as programs or used as headers. A test that needs a missing tool skips with a message, like tests that need the game. CI needs the packages installed, which is a change to `.github/` and so belongs to the maintainer (item 0).

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

Programs are counted by their `OrbShdr` marker. In every archive that count matches the count of `s_endpgm`.

What a first look shows, to be worked out and documented in `docs/formats/`:

- **DCX** (`docs/formats/dcx.md`): a big-endian header with the sizes (`DCS` chunk) and a `DFLT` method tag, followed by a zlib stream.
- **BND4** (`docs/formats/bnd4.md`): an archive with a file table, names and data offsets. The shader set has no nested archives.
- **Shader program** (`docs/formats/shader-binary.md`): each archive entry starts with a header (tag `Shdr`) that gives the offset of the first program's code. A program is GCN code, then a footer that ends in a 0x1C-byte block starting with the `OrbShdr` marker. The footer, together with the data after the last block, holds the metadata:
  - resource names (`g_Texture0`, `SS_ClampLinear`),
  - input semantics (`IN.tex0`, `TEXCOORD`),
  - the entry point name (only as the name of the outputs, so compute shaders don't carry it; #117).
  
  Some files hold more than one program.

Material on these formats from category 3 of the code sources may be used under its rules. Everything gets checked against the files.

`tools/shaderbnd` checks each archive's SHA-256 against a pinned table in a new file in `tools/common/`. It refuses other files. It writes to `analysis/shaders/<archive>/`:

- one file per program under `programs/`: code plus footer, as stored. The last program of an entry also holds the data after its footer block;
- every entry as stored under `entries/`, so the entry header and the trailing data stay available;
- `index.json`, with entry name, program index, offset in the entry and size (#114), and the program type, stage, code size and SHA-256 of the code (the **code hash**, #117); per entry, the resource, sampler, input and output names from the metadata (#117).

Nothing it writes is committed.

The code hash is the shader's identity everywhere: in corpus reports, in the translation cache, and later at run time, where the game gives us only the code address (`SPI_SHADER_PGM_LO_*`). Whether the footer follows the code in GPU memory, so the run time can find the code size the same way, is checked once the game boots. Until then the run time decodes as described below.

The eboot may contain shaders of its own, such as GNM's internal clear and copy shaders, or fetch shaders. The extractor also scans the eboot for the `OrbShdr` marker and reports what it finds (`--scan-eboot`, report only): 5,493 footer blocks of PS, VS, ES, LS and CS programs, most of them in the executable segment (`docs/formats/shader-binary.md`).

## Shader translator

```
code ─decode─► instructions ─cfg─► blocks ─analyze─► typed program ─structurize─► SPIR-V
                                                 ▲
                                       ShaderKey (pipeline state)
```

### Decoder and disassembler

- Decoding is table-driven, for every Sea Islands microcode format: SOP2, SOPK, SOP1, SOPC, SOPP, SMRD, VOP2, VOP1, VOPC, VOP3, VINTRP, DS, MUBUF, MTBUF, MIMG, EXP and FLAT. It handles literal constants and inline constants.
- The result is a list of `Instruction` values (opcode, format, operands, modifiers, size).
- A program can end in an early `s_endpgm` before other blocks. Decoding stops at the first `s_endpgm` that lies at or beyond the furthest branch target, or at the code size from the footer when it is known.
- The disassembler prints LLVM's assembly syntax.
- An unknown encoding is an error that names the offset and the word. It is never skipped.
- The opcode tables are written by hand from the ISA manual. They are code, not generated data. Regular families are built in code rather than listed: VOPC names, for example, come from the base (`cmp`, `cmpx`, `cmps`, `cmpsx`) × condition × type. Table rows are kept dense.

### Execution model

**One SPIR-V invocation is one GCN lane.** The registers of the wave become per-invocation variables:

- **VGPRs** are the lane's own values. When the program uses relative indexing (`v_movrel*`, `s_movrel*`, with the index in M0), they become an indexable array.
- **SGPRs** are copied into every lane. They are uniform in the hardware, and every lane computing the same scalar value gives the same result. M0 is an SGPR like the others.
- **Lane masks** (EXEC, VCC, and SGPR pairs that hold compare results or saved masks) are stored per lane as **this lane's bit**, a bool. A type pass decides which SGPRs hold masks and which hold values. An SGPR used as both is reported as an error until a shader needs it.
  - Bitwise operations on masks (`s_and_b64`, `s_or_b64`, `s_andn2_b64`, `s_and_saveexec_b64`, …) become boolean operations on that bit.
  - Mask constants other than 0 and −1, and 32-bit writes to `exec_lo`/`exec_hi`, take the bit at the lane ID.
- **The lane ID** is `LocalInvocationIndex & 63` in compute, and `SubgroupInvocationID` in the graphics stages.
- **Cross-lane operations** need the whole mask and use subgroup operations:

  | Operation | Translation |
  |---|---|
  | SCC after a mask operation; `s_cbranch_execz`/`execnz`/`vccz`/`vccnz` | any or all over the lane bits |
  | `s_bcnt1`, `s_ff1` and the other scalar operations on masks | ballot of the lane bits |
  | `v_readfirstlane_b32` | find the lowest lane whose EXEC bit is set (ballot, then lowest set bit), then broadcast from it with a dynamic lane ID (SPIR-V 1.5) |
  | `v_readlane_b32` | broadcast from lane *n* (ignores EXEC) |
  | `v_writelane_b32` | `lane_id == n ? s : v` (ignores EXEC) |
  | `v_mbcnt_lo_u32_b32`, `v_mbcnt_hi_u32_b32` | count of mask bits below the lane ID |
  | `s_wqm_b64` | any over the quad (quad broadcasts, or `OpGroupNonUniformQuadAnyKHR` with `SPV_KHR_quad_control`) |
  | `ds_swizzle_b32` | subgroup shuffle (gfx7 has no `ds_permute`/`ds_bpermute`) |

**Divergence is predication.** A GCN program's control flow is scalar, so the whole wave takes every branch together. Lanes diverge only through EXEC. The translator mirrors that:

- **Branch structure.** It comes from the scalar branches and becomes structured SPIR-V control flow. The branch conditions are uniform per subgroup by construction. The structurizer handles acyclic regions and natural loops. An irreducible graph falls back to a loop around a switch on a block index.
- **Vector results.** A VALU result is written as `select(active, new, old)`. The operation itself runs in every lane.
- **Mask results.** A mask destination is written as `active && result`, not as a select, because GCN clears the bits of inactive lanes. This covers every VALU mask destination: `v_cmp*` into VCC or SGPRs, the implicit VCC of VOP2 `v_add_i32`, `v_sub_i32`, `v_addc_u32` and `v_subb_u32`, VOP3 SGPR destinations, and `v_div_scale_*`. `v_cmpx*` sets EXEC = `active && result`.
- **Vector memory.** Every MUBUF and MTBUF access, load or store, runs only under `if (active && in_range)` (see Resources). An inactive lane may hold any address.
- **Scalar memory** (SMRD) runs in every lane, with no `active` term. The wave executes it even when EXEC is 0, its address is uniform and really used, and skipping it would leave stale SGPRs in some lanes.
- **Other side effects.** Exports, atomics and LDS writes run under `if (active)`.
- **Image operations.** Sampling runs unguarded, so derivatives stay available. Other image loads run unguarded too, with robust image access enabled. `robustImageAccess2` (`VK_EXT_robustness2`) returns 0 out of range like GCN. With only `robustImageAccess`, the value is undefined, which can differ for active lanes with out-of-range coordinates. Image stores and image atomics are side effects and run under `if (active)`.

**Pixel shaders** need four more rules:

- EXEC starts as "this fragment is covered", that is `!HelperInvocation`.
- `s_wqm_b64` (whole quad mode) turns the helper lanes of a quad back on for derivatives, as in the table above.
- At the final `exp … done`, a lane whose EXEC bit is false was discarded. It executes `OpDemoteToHelperInvocation` (core in SPIR-V 1.6 and Vulkan 1.3), so the fragment is not written.
- Implicit derivatives are only defined in uniform control flow. The branch conditions are uniform per subgroup, and that works on real drivers. Where `VK_KHR_shader_quad_control` is available, the translator declares `QuadDerivativesKHR` and `RequireFullQuadsKHR`, which make it defined by the spec.

**Execz skips and live-out scalars.** A forward `s_cbranch_execz` that skips a region in which scalars are defined and used afterwards is translated as a fall-through. The compiler emits these skips as an optimization, so executing the region with no active lane gives the same result. Without the fall-through, a subgroup whose lanes are all inactive would skip scalar work that the 64-lane wave runs.

`s_cbranch_vccz` and `s_cbranch_vccnz` are always real branches on any(VCC). On gfx7 they are also how uniform branches on float compares are made (there is no scalar float compare), and the skipped region then runs under full EXEC.

**Wave size.** A GCN wave has 64 lanes, but a Vulkan subgroup may be smaller: 32 on many GPUs, 8 or 16 on lavapipe.

- **Exact mode** requires a subgroup size of 64 (`VK_EXT_subgroup_size_control`, with `requireFullSubgroups`) in the stages listed in `requiredSubgroupSizeStages`. It matters mainly for compute: the PS4 doesn't define which vertices or pixels share a wave either.
- **Lane-local mode** computes cross-lane results over the subgroup instead of the wave.
  - In compute, ballot results keep their wave bit positions, and the bits of lanes in other subgroups read as 0. The wave position of bit *i* is `subgroup_base + i`, with `subgroup_base = lane_id - SubgroupInvocationID`.
  - `v_readfirstlane_b32` and `v_readlane_b32` convert the wave lane back to `lane - subgroup_base` before the broadcast. A lane outside the subgroup reads as 0.
  - This assumes that a subgroup covers consecutive values of `LocalInvocationIndex`. Vulkan doesn't guarantee it, but implementations do it. The execution tests check it once (`subgroup_base` is uniform per subgroup).
  - A device with `maxSubgroupSize` above 64 is not supported.

An analysis pass marks the shaders whose results can depend on the wave width: cross-lane results that are not uniform, and `s_barrier` under control flow that depends on EXEC (a hang risk). The corpus report counts them.

### Control flow

The scalar branches split a program into blocks (`build_cfg`), and the structurizer turns the graph into SPIR-V's structured control flow.

- **Blocks** start at the entry, at every branch target, and after every branch and `s_endpgm`. A block ends in a fall-through, an `s_branch`, a conditional `s_cbranch_*` (taken or next) or `s_endpgm`. A conditional branch to the next instruction is a fall-through. A branch that doesn't land on an instruction, control that runs off the end, and the unsupported branches (`s_cbranch_*fork`, `s_cbranch_join`, `s_setpc_b64`, the `s_cbranch_cdbg*` debug branches) are errors. `s_swappc_b64` is not a branch here: the fetch subroutine it calls is inlined.
- **Execz skips** (see above) become a fall-through when the skipped region, the blocks between the branch and its target, is entered only from the branch's block, is left only to the target, doesn't end the program, and writes scalars. "Writes scalars" means a scalar ALU or SMRD instruction (they all write SCC or a destination), or a scalar destination operand in another format. Whether the scalars are also used after the region isn't checked: that needs the implicit operands of every opcode, which the translator's passes know. A fall-through is correct either way, since running the region with no active lane gives the same result; it only costs the skip. The order in which skips are checked doesn't matter: a nested skip only branches within the outer region or to its target, and two overlapping skips block each other. `vccz` and `vccnz` stay branches.
- **Analysis** (`analyze`): reverse postorder of the reachable blocks, immediate dominators with the iterative algorithm of Cooper, Harvey and Kennedy, loop headers (the targets of back edges that dominate their source), merge nodes (more than one forward edge in), and whether the graph is reducible.
- **Structurizer** (reducible graphs). The nesting follows Ramsey's "Beyond Relooper": every block is emitted exactly once, either inline where its only forward predecessor branches to it, or, as a merge node, right after a scope opened in the code of its immediate dominator. A block's merge nodes nest by reverse postorder, the last one outermost. In SPIR-V:
  - A **scope** is a loop construct that runs once, its continue block unreachable. A branch to its merge node is a break, valid from any depth of selections inside it.
  - A **natural loop** is a loop construct around its header and the scopes that the header opens. All back edges continue to one continue block that branches to the header. Nothing breaks it: an exit continues inline inside the construct (an exit block with one predecessor), returns, or branches to an enclosing scope. Its merge block is unreachable unless a stored branch leaves through it.
  - A **conditional block** is a selection whose merge block is unreachable. Each arm is a break, a continue, a return, or the inlined successor.
  - A **branch out of several constructs** stores the depth of its target (plus 1) in a function variable and breaks the innermost construct. The merge block of each construct it leaves checks the variable: 0 continues with the merge node, otherwise the branch moves one construct out, and the target clears the variable. SPIR-V only allows breaks and continues to the innermost loop, so there is no shorter form.
  - The caller's prologue block is the function's entry block, so a loop header is never the entry.
  - An irreducible graph is a `GcnError` for now. The fallback (a loop around a switch on a block index) comes in a third part of #124, split off to keep each PR within the size limit.

### Per-draw data

Each draw or dispatch passes one push constant: a buffer device address pointing to the **draw block**. It holds:

- `user_data[16]`, the stage's `SPI_SHADER_USER_DATA_*` or `COMPUTE_USER_DATA_*` values, which the program finds in its first SGPRs;
- `address_bias`, so that device address = guest GPU address + bias;
- `code_address`, the guest address of the program, for `s_getpc_b64`.

The layout is `user_data` at offset 0, `address_bias` at 64 and `code_address` at 72, 80 bytes in all (constants in `tests/gpu/runner/compute_runner.hpp` until the translator, item 6c, owns them). The shader reads it through `PhysicalStorageBuffer` pointers.

For the first wave, the guest's GPU-visible memory is **one contiguous imported range**, so a single bias is enough. The execution tests allocate one buffer and set the bias to match. A page table can replace the bias later without changing the shaders' view.

### Resources and memory

GCN shaders get their resources as descriptors in SGPRs:

| Descriptor | Meaning |
|---|---|
| V# | buffer |
| T# | image |
| S# | sampler |

The descriptors come from user data directly, or through `s_load_*` from a pointer that itself comes from user data. A dataflow pass records the **source** of every descriptor that a memory instruction uses: user-data slot *k*, or a load at offset *o* through the pointer in slot *k*, nested as deep as needed. The list of sources is part of the shader's interface.

- **Buffers and scalar memory** are read and written through buffer device addresses (`VK_KHR_buffer_device_address`, `PhysicalStorageBuffer` in SPIR-V).
  - The base address and the other V# fields are data, read at run time.
  - MUBUF, MTBUF and `s_buffer_load_*` check the range the way the hardware does: from `num_records`, the stride, and the `idxen`/`offen`/`addr64` modes. Out-of-range reads return 0, and out-of-range writes are dropped. The game may rely on that. `s_load_*` without a V# has no range check.
  - Typed formats (MTBUF, or the format of a V#) are converted in the shader from raw dwords.
- **Vertex fetch** is ordinary buffer loading. PS4 vertex shaders load their attributes themselves, through a fetch subroutine called with `s_swappc_b64`. The translator inlines the fetch subroutine, which it receives as a second code input, so Vulkan's vertex input state stays empty. The eboot generates the fetch code at run time and puts its address into user-data dwords 0–1 of the vertex stage (VS, ES or LS), so the run time reads it from there at draw time and decodes it up to its `s_setpc_b64`. Offline, the harness can call the eboot's generator to get real fetch shaders ([gnm.md](../formats/gnm.md#fetch-shaders)).
- **Images and samplers** need bindings. Each T# and S# source gets an index into descriptor arrays. The SPIR-V image type needs dim, arrayed, multisample and the sampled type at compile time, but these come from T# memory, not from registers. They are therefore part of the ShaderKey and checked at draw time. A mismatch means a new translation.
- **Scratch** (private memory: swizzled MUBUF accesses through the scratch descriptor) becomes a per-invocation array. Its size comes from `SPI_TMPRING_SIZE` or `COMPUTE_TMPRING_SIZE` through the ShaderKey, because the array size is fixed at compile time. A size above a fixed limit is reported as unsupported.
- **LDS** becomes a `Workgroup` array. That storage class exists only in compute. The translator ignores the LDS clamp in M0. **GDS and ordered append** are reported as unsupported until a shader needs them.
- **Fork and join.** `s_cbranch_*fork` and `s_cbranch_join` (SI/CI) are reported as unsupported.

### Stage interfaces and ShaderKey

The register state a program starts with depends on pipeline registers, and so do the stage's inputs and outputs:

| Stage | Inputs | Outputs |
|---|---|---|
| VS | user data in SGPRs; vertex and instance index in VGPRs, per `SPI_SHADER_PGM_RSRC*` (`VGPR_COMP_CNT`) | `exp pos0` → `Position`, `exp param<n>` → `Location n` |
| PS | `SPI_PS_INPUT_ENA/ADDR` decide which VGPRs are set; `v_interp_p1`/`p2` pairs on attribute *n* → an interpolated input at the location from `SPI_PS_INPUT_CNTL_n`, its qualifier taken from the barycentrics it reads; `v_interp_mov` → `Flat` | `exp mrt<n>` → output *n*, converted per `SPI_SHADER_COL_FORMAT`; `exp mrtz` → `FragDepth`, plus stencil and sample mask per `SPI_SHADER_Z_FORMAT` |
| CS | user data; thread and group IDs per `COMPUTE_PGM_RSRC2` and `COMPUTE_NUM_THREAD_*` | memory only |

`ShaderKey` holds everything the generated SPIR-V depends on besides the code:

- the stage and the register values above;
- the float mode bits from `SPI_SHADER_PGM_RSRC1_*` or `COMPUTE_PGM_RSRC1` (`FLOAT_MODE`, `DX10_CLAMP`, `IEEE_MODE`), which change min/max, NaN and denormal behavior;
- the image type class of each T# source (above);
- the scratch size (`SPI_TMPRING_SIZE`, `COMPUTE_TMPRING_SIZE`).

The translation cache key is code hash + fetch code hash + ShaderKey + translator version. The cache lives in `$XDG_CACHE_HOME/bloodborne-linux/shaders/`. Offline, keys come from the footer metadata where it carries them, otherwise from test defaults.

Other stages (LS, HS, ES, GS for tessellation and geometry shaders) come later. The corpus uses all of them: 850 LS, 404 HS, 1,699 ES and 1,701 GS programs (with their copy shaders) of 20,218, plus 469 domain shaders that run as VS (`docs/formats/shader-binary.md`).

### SPIR-V builder

`bbgcn` has its own small module builder that emits the word stream directly, with the enum values from the Khronos SPIR-V headers. The baseline is Vulkan 1.3 (demote to helper invocation, `robustImageAccess`, subgroup size control are core there), and the target is SPIR-V 1.6. Every translated module must pass `spirv-val --target-env vulkan1.3` in the tests. Lavapipe supports all of it. An optimizer (`spirv-opt`) can come later, behind the cache.

### Interface

```cpp
namespace bb::gcn {

enum class Stage { vertex, pixel, compute };

class GcnError : public std::runtime_error { /* offset() and word() of the failing instruction */ };

enum class Until { program_end, code_end };          // the stop rule above, or every word
std::vector<Instruction> decode(std::span<const std::uint32_t> code,    // throws GcnError
                                Until until = Until::program_end);
std::string disassemble(const Instruction& inst);    // LLVM syntax, one line
std::optional<std::uint32_t> branch_target(const Instruction& inst);   // byte offset
bool is_endpgm(const Instruction& inst);

struct ShaderKey { Stage stage; /* see "Stage interfaces and ShaderKey" */ };

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

Decoder (`src/gpu/gcn/gcn.hpp`, #115):

- An `Instruction` holds its byte offset, its size in dwords (with the literal), the format, the format's opcode field, LLVM's mnemonic, and up to 2 destination and 4 source `Operand`s in the order of LLVM's syntax.
- An `Operand` has a kind, its size in dwords, the 8-bit operand code and a 32-bit value. The kinds so far: scalar register (by operand code, so the special registers keep their codes), inline constant (with its 32-bit pattern; on a 2-dword operand an integer constant sign-extends it, and a float constant means the f64 of the same number, so `value` holds the f64's high dword and the low dword is 0), literal, and the immediates of SOPK, SOPP and SMRD (hex value, count, branch offset, hardware register, wait counters, message, dword offset). The vector formats add their kinds and modifiers.
- The opcode tables are in `opcodes.cpp`, one per format, indexed by opcode.
- Register ranges of more than one dword must be aligned, to 2 dwords for pairs and to 4 above, as LLVM requires. A misaligned range is a `GcnError`, as is a reserved operand code or an operand code that the instruction can't use (a 64-bit `m0`, a constant as SMRD offset register). Choices that LLVM restricts per instruction but the operand field allows, such as `m0` as an SMRD destination, are decoded.
- An SMRD offset field of 255 with the immediate bit clear is a 32-bit literal offset (CI).

SPIR-V builder (`src/gpu/gcn/spirv_builder.hpp`, #122):

- `SpirvBuilder` collects one module and returns its words. Each call appends to the section of the logical layout it belongs to, so callers may declare a type or a decoration in the middle of a function.
- Types and constants are shared by value: asking twice gives the same id. Arrays, runtime arrays and structs get a new id on every call, because their layout decorations belong to one use: a laid-out array in a buffer block and a plain one for registers or LDS must stay distinct types. Capabilities, extensions and extended-instruction imports are recorded once each.
- `variable` declares a global in any storage class but `Function`. `local_variable` may be called anywhere in a function, and its `OpVariable` goes to the start of the first block, where SPIR-V requires it.
- `op` takes an optional result id from `id()`, for an instruction that an earlier `OpPhi` already names (a loop's back-edge value).
- `op` and `op_void` emit any instruction inside a block, with enum operands through `spirv_word`. Misuse (an instruction outside a block, a block without a terminator, nested functions, a parameter after the first block) throws `std::logic_error`. Validity is checked by `spirv-val`, not by the builder.
- Enum values come from `spirv/unified1/spirv.hpp11`.
- It has the module sections, capabilities, extensions, extended-instruction imports, memory model, entry points, execution modes, names, decorations and member decorations, the scalar, vector, array, runtime-array, struct, pointer and function types, scalar constants (`constant` for any type, helpers for bool, u32, i32 and f32), composite and null constants, global and local variables, functions and blocks.
- Tests: `tests/gpu/gcn/spirv_val.*` pipes a module to `spirv-val --target-env vulkan1.3 -` and returns its messages. The tests skip without the tool. A validated compute module covers what the translator needs there: the draw block through a push constant and a `PhysicalStorageBuffer64` address, subgroup ballot, broadcast with a dynamic lane and shuffle, a `Workgroup` array, a `GLSL.std.450` instruction, float and signed constants, an extension, a function call with a parameter, a selection and a loop with a phi. A negative test checks that a module without a needed capability is rejected. The word-encoding tests build fragments that `spirv-val` would reject (no capability, undefined ids), so they compare words only; every builder call they make also appears in the validated module.
- The image, sampler and sampled-image types and the validated vertex and fragment modules (with `OpDemoteToHelperInvocation`) moved to #129 (listed in a comment there), to keep #122 within the PR size limit.
- The `SPV_KHR_quad_control` names (`QuadControlKHR`, `QuadDerivativesKHR`, `RequireFullQuadsKHR`) are newer than some distributions' headers, so item 8a checks CI's headers before using them.

Control flow graph (`src/gpu/gcn/cfg.hpp`, #124):

- `build_cfg` returns the blocks in program order, block 0 first. A `Block` holds the byte offset and index of its first instruction, its instruction count (the branch included), its `Exit` (`next`, `jump`, `conditional`, `end`) and the block indices `taken` and `next`. `successors` lists `taken` before `next`. The translator emits nothing for a block's branch instruction: its effect is the exit, so a skip that became a fall-through simply ends the block.
- `analyze` returns a `CfgAnalysis`: `rpo`, `order` (each block's position in `rpo`), `idom`, `loop_header`, `merge_node`, `reducible`, and `dominates(a, b)`. Unreachable blocks have position and dominator `CfgAnalysis::none`.
- Tests: synthetic programs of SOPP branches and `s_nop` placeholders, decoded from words encoded by hand. SOP1 is decoded in parallel (#115), so a scalar write is a placeholder turned into an `s_mov_b32` with a scalar destination.

Structurizer (`src/gpu/gcn/structurize.hpp`, #124):

- `structurize(cfg, builder, emitter)` emits into the current function. A caller block must be open (the prologue); the structurizer ends it with a branch and returns with no block open.
- `BlockEmitter` has two callbacks: `body(block)` emits the block's instructions into the current SPIR-V block, and `condition(block)`, for a conditional exit, emits the branch condition after the body and returns its bool id (true takes the branch). Each is called once per reachable block.
- Tests: synthetic programs pass `spirv-val`, and a small interpreter for the instructions the structurizer emits runs each module with scripted branch outcomes. The blocks it visits must match a walk over the graph. The cases are if/else, nested loops, early returns and breaks out of a loop to an outer merge node, and a continue from inside a scope. A randomized version over a few hundred random programs is planned for the third part, where irreducible graphs no longer throw.

## Offline test strategy

Four levels. The first three need neither a running game nor a GPU.

1. **Unit tests** (CI): the decoder per microcode format, with words encoded by hand from the ISA manual; the disassembler text; the SPIR-V builder through `spirv-val`. The extractor gets synthetic DCX and BND4 files, so CI covers it too.
2. **Corpus tests** (local, need the game). They skip without `BB_GAME_DIR`, and with "run `tools/shaderbnd`" when `analysis/shaders/` is missing. Over every extracted program:
   - It decodes completely, has exactly one final `s_endpgm`, and its size matches the footer.
   - **Round trip through LLVM:** our disassembly of each instruction, assembled by `llvm-mc -triple=amdgcn -mcpu=bonaire -show-encoding`, gives the original words. LLVM's disassembler doesn't support gfx6/7, so the round trip goes through its assembler. Instructions whose LLVM syntax can't express an encoding exactly (unused fields, alternative encodings) go on a short, reviewed list of known differences.
   - It translates with the default key, and the module passes `spirv-val`.
   - A report counts opcodes, untranslated instructions and wave-size-dependent shaders. It goes to `analysis/`, not into the repo.
3. **Execution tests** (CI with lavapipe):
   - Small GCN programs are written in assembly by us under `tests/gpu/programs/`. `llvm-mc` assembles them, and the words are committed next to the source as hex text (`.hex`, one instruction per line). That way CI doesn't need LLVM, and the hooks don't mistake the files for game binaries. When `llvm-mc` is installed, a test checks that the hex matches the source. The programs stay small, because they count toward the PR line limit.
     - Format (#123): `<name>.s` is LLVM assembly for `bonaire`, with `;` comments. `<name>.hex` has one line per instruction, each word as 8 lower-case hex digits, the words of a line separated by one space (2 words for a literal or a 64-bit encoding; gfx7 has nothing longer), and a final newline. `smov_literal` is the example.
     - Branches take numeric offsets, not labels: `llvm-mc -show-encoding` leaves a label's fixup unresolved in the encoding it prints, so `assemble_file` rejects a source that uses one.
     - `tests/gpu/gcn/programs.*` (in `bbgcntest`) reads the words. `GcnPrograms.WordsMatchSource` assembles each source with `assemble_file` and compares, and skips without `llvm-mc`.
   - Each program is translated and run on lavapipe with input buffers. Its output is compared with values worked out by hand from the ISA manual. Cross-lane results are worked out for the device's subgroup size, not for 64.
   - They cover ALU semantics (output modifiers, clamping, 24-bit multiplies), EXEC predication and loops, scalar and buffer memory with range checks, and LDS.
   - Denormal tests run only where the driver's float-controls properties allow GCN's mode, otherwise they skip. Transcendental results are compared with a tolerance in ULPs.
   - A graphics test renders a VS and PS pair into an offscreen image.
   - **Compute runner** (`tests/gpu/runner/`, the static library `bbcomputerunner`, #123). `tests/gpu/CMakeLists.txt` builds it only when `find_package(Vulkan)` finds Vulkan, so CI without the packages still builds everything else.
     - `ComputeRunner::create(why)` makes a Vulkan 1.3 instance and picks the device whose `driverID` is `VK_DRIVER_ID_MESA_LLVMPIPE`. It sets `VK_LOADER_DRIVERS_SELECT=*lvp*` first, unless the environment already sets it, so the loader doesn't load and initialize the other installed drivers. It needs `shaderInt64`, `bufferDeviceAddress`, `subgroupBroadcastDynamicId` (a broadcast from a lane computed at run time), `subgroupSizeControl` and `computeFullSubgroups`, and the basic, vote, ballot and shuffle subgroup operations in compute, and enables the features on its device.
     - Without an instance, without lavapipe or without one of these, it returns nothing and `why` names what is missing; the tests then skip with that text. While the filter is `*lvp*`, the loader fails `vkCreateInstance` when lavapipe is missing, even with other drivers installed, so that failure is reported as a missing lavapipe driver; under another filter from the environment it is reported as a missing Vulkan instance. Two tests point `VK_DRIVER_FILES` at a missing file to check both wordings.
     - `info()` reports the device and driver names, the subgroup size with its range, and the 32-bit float controls (denormals preserved or flushed, RTE and RTZ rounding, signed zero, Inf and NaN preserved). Lavapipe from Mesa 26.2 (LLVM 22, 256 bits) reports subgroup size 8 (8 to 8), and neither denormal mode, so the denormal tests will always skip there.
     - Mesa allocates one-time state under `pthread_once` and never frees it, and the loader unloads the driver before LeakSanitizer runs, so the frames have no module name. The runner defines `__lsan_default_suppressions()` with `leak:pthread_once` for the binaries that link it.
     - `guest_memory(base, size)` allocates the guest's GPU-visible memory: one zeroed, host-visible and host-coherent buffer with a device address, at guest address `base`, replacing the previous one. It returns the mapped bytes. A size of 0 throws `std::invalid_argument`.
     - `run(spirv, draw, groups)` writes the draw block into a buffer of its own, with `address_bias` = device address of the guest memory − `base` (modulo 2^64). Its device address is the only push constant, 8 bytes, and there are no descriptor sets. It builds the pipeline (entry point `main`), dispatches, and waits on a fence (30 s), behind a barrier from shader writes to host reads. Called before `guest_memory`, it throws `std::logic_error`.
     - The test module is written with `SpirvBuilder` and checked with `spirv-val` when it is installed. It reads the draw block like a translated program: the guest addresses of its output and input come from user data, it adds a user-data value to every input word, and invocation 0 writes `SubgroupSize` and `code_address`. The guest addresses lie above 2^32, so the high words count. The test runs it twice, the second time on new guest memory at another base, so the reset of the command pool and fence is covered. The pipeline asks for no subgroup size, so the test accepts any power of two in the device's range.
   - The exact wave-64 mode runs locally on an AMD GPU.
4. **Replays** (later wave, after M3):
   - Captures of real frames, as PM4 streams plus the memory they reference, go to `analysis/captures/`. They come from our runtime, or as reference frames from an emulator run on the game with call logging off (see [clean-room.md](../clean-room.md#code-sources)).
   - The captures are replayed through `bbpm4` and `bbvk`, and the images are compared.

Committed test data is only our own: assembly sources, their assembled words, and expected values. Nothing extracted from the game is committed.

## Command buffers and state (later wave, sketch)

- **Parsing.** The `libSceGnmDriver` submit functions hand `bbpm4` the command buffers and constant-engine buffers. Parsing runs on a GPU thread, never on the calling game thread. What the eboot writes and submits is in [gnm.md](../formats/gnm.md).
- **Constant engine.** The game uses it: each DCB comes with a CCB, and the two streams synchronise through the CE/DE counters, so `bbpm4` runs both.
- **Compute queues.** The game maps compute queues on its own ring buffers and reports the write offset through an import. `bbpm4` parses those rings too.
- **Addresses.** Packets carry the game's own virtual addresses, and command buffers hold data the GPU reads (descriptors, constants, indices). Command buffer memory is GPU-visible memory like the rest.
- **One format.** Imports that write packets into a game buffer write standard PM4 (per the SI programming guide), so the parser sees one format.
- **Register writes.** `SET_CONTEXT_REG`, `SET_SH_REG`, `SET_UCONFIG_REG` and `SET_CONFIG_REG` update a mirror of the register file.
- **Draws and dispatches** take a snapshot of the state the backend needs: ShaderKey, render targets, blend, depth and raster state, and user data.
- **Unknown packets** are logged once per opcode; the eboot writes two opcodes that have no name yet (0x8C, 0x8D), and the names of the others still need checking against an AMD PM4 header ([gnm.md](../formats/gnm.md#open-questions-and-next-steps)). `NOP` packets with a payload go to a hook list, because the driver may use them as markers. The GNM code tags some packets with `NOP`s whose payload starts with `0x6875xxxx`.
- **Register names and offsets** come from the AMD register guide. The Mesa register database (code sources, category 1) may be used to generate a table, as generated data in its own PR.
- **GPU memory.** The guest's GPU-visible memory (libkernel direct memory, M3) is imported into Vulkan with `VK_EXT_external_memory_host` where the device allows it. That gives device addresses without copies. Such memory stays host memory, so on a discrete GPU every read crosses PCIe. A device-local cache with write tracking will be needed for vertex and constant data.
- **Textures and render targets** are cached by address and descriptor. Tiled ↔ linear conversion uses AMD's addrlib (code sources, category 1, MIT), copied into `third_party/` in its own PR.
- **Clip space.** Clip-space conventions (`PA_CL_CLIP_CNTL`, depth range, viewport orientation) are decided with the backend.
- **Flip.** `libSceVideoOut` flips present through a Vulkan swapchain.

## First wave (offline)

Each item is one issue, at most 500 lines of code and tests. Every PR that first uses a tool or header adds its row to `docs/references.md`: LLVM (`llvm-mc`), SPIRV-Tools, SPIRV-Headers, Mesa's lavapipe, the Vulkan loader.

| # | Item | Depends on |
|---|---|---|
| 0 | **Maintainer: CI packages and layout.** Install `spirv-headers`, `spirv-tools`, `libvulkan-dev` and `mesa-vulkan-drivers` in `.github/workflows/ci.yml`, and add `src/gpu/` and `tools/shaderbnd/` to the layout in `AGENTS.md`. Both are guardrails. | – |
| 1 | **DCX and BND4 formats and `tools/shaderbnd`.** Format notes, pinned archive hashes in a new `tools/common` file, extraction into `analysis/shaders/`, synthetic unit tests. | – |
| 2 | **Shader binary footer.** Format note; stage, code size and resource names in `index.json`; the eboot scan for embedded programs. | 1 |
| 3a | **Decoder core and scalar formats.** `bbgcn` skeleton, `Instruction`, SOP1/SOP2/SOPK/SOPC/SOPP/SMRD with disassembly, and the `llvm-mc` round-trip test helper. | – |
| 3b | **Vector ALU formats.** VOP1, VOP2, VOPC, VOP3, VINTRP. | 3a |
| 3c | **LDS and flat formats.** DS, FLAT. | 3a |
| 3d | **Buffer, image and export formats.** MUBUF, MTBUF, MIMG, EXP. | 3a |
| 4 | **Corpus decode test.** Round trip of every program through `llvm-mc`. It also answers whether the corpus uses CI-only instructions. | 2, 3b, 3c, 3d |
| 5 | **SPIR-V builder** and `spirv-val` test support. | 0 |
| 6a | **Lavapipe compute runner.** Device setup, buffers, the draw block, and the test program format (assembly and hex). | 0, 5 |
| 6b | **CFG and structurizer.** Tested on synthetic graphs through `spirv-val`. | 3a, 5 |
| 6c | **Translator core.** Mask typing, predication, the SALU and VALU subset, the compute stage, and execution tests. | 3b, 6a, 6b |
| 7a | **Descriptor sources and untyped buffer memory.** SMRD and raw MUBUF with range checks, scratch. | 3d, 6c |
| 7b | **Typed buffer formats.** MTBUF and formatted MUBUF. | 7a |
| 7c | **LDS.** DS operations, `Workgroup` storage, `ds_swizzle_b32`. | 3c, 6c |
| 8a | **Graphics stages.** VS and PS interfaces, exports, interpolation, discard and demote, and an offscreen render test on lavapipe. | 7a |
| 8b | **Image sampling.** MIMG sample operations, image and sampler bindings, the image type class in the key. | 8a |
| 8c | **Other image operations.** Loads, stores, gather and atomics. | 8b |
| 9 | **Corpus translation report.** Translate everything, run `spirv-val`, record coverage. It grows as later items land. | 4, 6c |
| 10 | **GNM survey (research).** Covers how the eboot builds and submits command buffers, what the `libSceGnmDriver` imports do (behavior only, named by role), and where fetch shaders come from. Written up in `docs/formats/gnm.md`. It works from the eboot and the AMD documents only. Reading an emulator's GnmDriver HLE code would put `libSceGnmDriver` into the exposure log, so ask the maintainer first. | – |

Items 1, 3a and 10 can start at once. Items 5 and 6a wait for item 0.

## Open questions

- Does the corpus use CI-only instructions? Item 4 answers this.
- Does the footer follow the code in GPU memory? This is checked at run time.
- Where do fetch shaders come from? Answered by item 10: the eboot generates them at run time ([gnm.md](../formats/gnm.md#fetch-shaders)).
- How often do shaders depend on the wave size? Item 9 counts this.
- ~~Does the game use the LS/HS/ES/GS stages?~~ Yes (item 2, #117): the footer's program type shows 850 LS, 404 HS, 1,699 ES and 1,701 GS programs, plus a copy shader per GS and 469 domain shaders on the VS stage. Tessellation and geometry shaders are never combined. The eboot holds no GS or HS programs.
- Which clip-space conventions apply? This is decided with the backend.

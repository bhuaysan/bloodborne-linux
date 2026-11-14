# Translating the shader corpus without the game running

R3's acceptance check translates every distinct program of the game's shader archives with the translator derived from shadPS4 (`ShaderCorpus` in `tests/shadps4/corpus_test.cpp`, #417). The translator is written for a running GPU, so this note records what it reads of that GPU and how the corpus replaces it. The test decides whether a program translates and validates. It says nothing about whether the translation renders correctly: that is R4's check against the reference.

## What the translator reads

| Input | When | Source at run time | Source in the corpus |
|---|---|---|---|
| `RuntimeInfo`: float modes, register counts, user-data count | translating | program registers | the register block of the entry header (`RSRC1`, `RSRC2`) |
| `RuntimeInfo`: pixel inputs and export formats, Z and mask exports | translating | context registers | the block's `SPI_*` and `DB_SHADER_CONTROL` |
| `RuntimeInfo`: compute thread counts, LDS size, thread-group IDs | translating | compute registers | the block's `NUM_THREAD_*` and `RSRC2` |
| `RuntimeInfo`: ES/GS ring sizes, GS vertex count and primitives, tessellation strides and modes | translating | draw state | fixed values, see below |
| The copy shader of a geometry program | translating | the VS register | the entry's second program |
| The fetch shader of a vertex program | translating (it is parsed, not translated) | user data pointer into guest memory | an empty one |
| The V# of tessellation constants | translating | user data, then guest memory | a block of fixed constants |
| Sharps and SRT tables | translating: `FlattenExtendedUserdataPass` runs its generated walker and fills `flattened_ud_buf` | guest memory | none, every load reads 0, except in hull and domain programs (below) |

## Decisions

**Registers from the file, the rest fixed.** The entry header holds the registers the program was compiled with, so those are real. Draw-time state doesn't exist in the files. The fixed values are upper bounds or the common case, because the question here is whether the translator handles the instructions, and the instruction mix doesn't depend on them.

**Null sharps through the walker's own fault path.** The SRT walker dereferences guest pointers, and upstream registers a fault handler that patches its loads to read 0 when a pointer is bad. The corpus keeps user data at 0 and connects that handler, through `Core::Signals`, so the real translator path runs and every resource reads as null. Hull and domain programs are the exception: every user data register holds the address of the block with their tessellation constants, so their walker loads read that memory and their sharps aren't null. Every dword of those sharps is that address, so its bits set their formats and flags, a sampler's degamma bit among them. The block therefore lies at a fixed address: where `mmap` put it, address randomization changed up to 40 domain shaders from run to run (#501). The alternative, a block of fake descriptors, would pick image types and formats at random and hide nothing that null sharps don't.

The cost is that image and buffer types follow a null sharp. A shader whose SPIR-V depends on the real T# is checked for one type only. R4 sees the real ones.

**A copy shader counts as parsed.** The translator reads a copy shader while translating its geometry shader and never emits a module for it. So a copy shader counts when `ParseCopyShader` accepts it.

**Tessellation constants in two forms.** A program reaches its V# either inline in user data or through a pointer, and the files don't say which. No single layout of the user data serves both, so a tessellation program that faults on the first form is run again with the second.

**One process per program.** Assertions become `bb::shader::TranslationFailure`, but libstdc++ bounds checks, Sirit's `abort` and bad pointers can't be caught. A forked child per program keeps any of them to that program.

## Result

Of the 3,832 programs, 3,793 translate to a module that passes `spirv-val --target-env vulkan1.3` and 30 are copy shaders that `ParseCopyShader` accepts (2026-11-09, debug and optimized builds, the device profile of the reference GPU). Nine don't translate, and the test lists exactly these programs by the SHA-256 of their code, each with its cause. Another program that fails, a listed one that fails for another cause and a listed one that stops failing all fail the test:

| Programs | Stage | Cause |
|---|---|---|
| 4 | hull | `HullShaderTransform` asserts on a tessellation factor store whose address isn't in the form it expects |
| 2 | geometry | `ParseCopyShader` asserts on an `s_lshl_b32` whose operands are not inline constants, in the copy shader of a stream-out program |
| 2 | copy shader | the same copy shaders on their own |
| 1 | pixel | a bug in the translator: `resource_patching_pass.cpp` adds a sampler only for `ImageSampleRaw`, but `EmitImageQueryLod` indexes `ctx.samplers` anyway. Out of bounds, and a crash with or without checked containers |

A program is identified by its code. 21 of them come with more than one register block, and only one block is translated.

A per-program comparison with the reference isn't possible: shadPS4 only translates what a run reaches. The reference run didn't crash, but it isn't known whether it reached any of these nine. R7 will show whether the game needs them.

The per-program report goes to `analysis/shadps4-corpus-report.txt` and is never committed.

## Translating ahead of time (#512)

**No module is translated ahead of time from the archives, so there is no `bb-shader-cache` tool.** The renderer reuses a module only when its `StageSpecialization` equals the one of the draw: the runtime information (render-target formats, pixel inputs, vertex outputs, the register settings), the fetch shader, the first binding, and the formats and types of every bound V#, T# and sampler. Those come from the game's state at draw time and from guest memory, which the archives don't hold. A module the corpus translates with its fixed state could only be stored under a key the game never looks up, or under the game's key with SPIR-V the GPU thread would not have produced.

`shadps4_shader_cache_match_test` measures this. Given the cache of a game run (`BB_SHADER_CACHE_DIR`), it translates the corpus program of every module in it with the corpus's state and the run's device profile and compares the specialization the renderer would compare, and the SPIR-V bytes. On 2026-11-14, with the caches of two cold runs (RADV):

| Run | Modules | From the archives | Key matches | From programs in the executable |
|---|---|---|---|---|
| new-game-name to the opening movie (#507's run) | 81 | 38 | 0 | 43 (all 16 compute modules) |
| start to the title (`20261114-032142-bb-run-new-game-name`, the pad did not reach the game) | 38 | 2 | 0 | 36 |

Every pixel shader from the archives differs in its render targets and its buffer or image sharps, 11 of 19 also in pixel state. Every vertex shader differs in its fetch shader and its first binding (the pixel stage's bindings come first), most also in buffer sharps. Five pixel shaders give the same SPIR-V under a different key, which the renderer would not find. Half the modules come from programs embedded in `eboot.bin`, which the corpus does not read at all.

The first run's cost is not small once the game gets past the title. A cold run through the name dialog into the intro (`20261114-033430-bb-run-new-game-name`) translated 369 shaders and built 293 pipelines in 2.4 s in all, 553 ms of it within about 15 s when the intro starts (`Pipeline cache: ... 2403 ms spent building them`). From the second run on, #507's cache loads every module and pipeline the game used. A cold first run needs another way to keep this work off the frame: #521 creates graphics pipelines on worker threads (`docs/design/renderer.md`).

The vertex shaders also differ in their register settings, so `runtime_info_of` reads a vertex program's register block wrongly. It does not change the corpus test's result, and is left as it is.

# GNM: how the eboot builds and submits GPU work

What the eboot does with the GPU, as far as its own code shows: the command buffer object, the PM4 packets the statically linked GNM code writes, how buffers reach the GPU, how shaders are bound, where fetch shaders come from, and what each of the 50 `libSceGnmDriver` imports does. Design item 10 of [graphics.md](../design/graphics.md); the second wave (PM4 parser, submission) is planned from this note.

Sources: the eboot of CUSA03173 patch 1.09 (disassembled with `objdump` over byte ranges, call sites found by scanning for `call`/`jmp rel32` to the PLT stubs and to the functions in `eh_frame`) and AMD's public documents listed in [references.md](../references.md): the Southern Islands programming guide for the PM4 header format, the packet descriptions and the config, SH and context register bases; the Sea Islands register reference for register addresses and fields; the Sea Islands ISA guide for instruction encodings. No emulator code and no driver binary was read. The driver module itself (`libSceGnmDriver_padebug.prx` in the eboot's `DT_NEEDED` list) is firmware and not in the dump, so everything about the imports comes from their callers.

Addresses are eboot link addresses. Register names are those of the register reference, which lists every register named in this note unless the text says otherwise.

**The packet names are unverified.** The opcode numbers are what the eboot writes. The SI programming guide describes packets by name and body but gives no opcode numbers, and no logged source pairs numbers with names. The pairings in the table below come from general knowledge of AMD's open-source PM4 headers, not from a source read for this note. They hold only once checked against one of those headers (see [open questions](#open-questions-and-next-steps)). Names that the SI guide doesn't describe at all are marked *not in guide*. Each packet's length and operands were checked against the writer that emits it, unless the row says otherwise.

## Summary

- **GNM is linked into the eboot.** All PM4 packets are written by eboot code into game memory. The driver imports only add a few fixed-size blocks (default state, shader registers, debug markers) into the same buffers and submit them.
- **Submission** passes arrays of (address, size) pairs: draw command buffers (DCB) and constant command buffers (CCB, constant engine). Addresses are the game's own virtual addresses; a packet that points into a command buffer uses the same pointer the CPU wrote through.
- **The constant engine is used.** The CCB writes descriptors into CE RAM and dumps them to memory; DE and CE synchronise through the CE/DE counters.
- **Compute queues are used.** The game maps compute queues on its own ring buffers, writes `INDIRECT_BUFFER` packets into the ring and reports the write pointer through an import.
- **Fetch shaders are generated at run time** by eboot code (0x10877c0), from the vertex shader's input-usage table. The address goes into the first two user-data SGPRs of the vertex stage (VS, or ES/LS when those run the vertex shader), where the shader's `s_swappc_b64 s[0:1], s[0:1]` finds it.
- **Of the 50 imports, 30 are reachable** from game code, 20 are not (their wrappers have no callers). Two of the reachable ones only matter when GPU profiling is on, which the HLE reports as off.
- **Two packet opcodes have no name yet** (0x8C, 0x8D). The parser has to log them until their meaning is known.

## Where the code lives

| Range | Contents |
|---|---|
| 0x2bc04a8–0x2bc07b8 | PLT stubs of the 50 imports (`jmp [rip+…]`, 16 bytes apart) |
| 0x10899f0–0x1089c4f | Thunks for 31 of the 50 imports: a `jmp` to the stub, or a 2–4 instruction wrapper that converts the result to a bool. The other 19 have no thunk: packet writers outside this range call their stubs directly (default state, stage registers, markers, `1qXLHIpROPE`) |
| 0x1071270 | Command buffer initialisation (below) |
| 0x1072870–0x1078833 | Graphics packet writers, one function per packet or packet group |
| 0x1078930–0x1084807 | Performance counter writers (only reached when profiling is on) |
| 0x10877c0, 0x1087450–0x1087710 | Fetch shader generator and its input builders |
| 0x1089c50–0x108bc3e | Compute-side packet writers. Many set the shader-type bit as a constant (for example `SET_SH_REG`, `SET_QUEUE_REG`, `DISPATCH_DIRECT`, `INDIRECT_BUFFER`, `RELEASE_MEM`, `EVENT_WRITE_EOS`, `REWIND`, 0x8D); others leave it clear or take it from the object's header bits (for example 0x8C, `SET_UCONFIG_REG`, `SET_CONFIG_REG`, `WRITE_DATA`, `EVENT_WRITE`, `NOP`, `DMA_DATA`, `WAIT_REG_MEM`, `ACQUIRE_MEM`, 0x39, 0x86). Neither list is complete. Some write bodies that differ from the graphics writer of the same opcode (0x8C, 0x8D) |
| 0x108c7b0–0x108ca00 | Compute queue object: map, unmap, submit to ring |
| 0x108d000–0x1092000 | State cache layer: per-stage shader binding, resource tables, constant engine use |
| 0x11fa000–0x11fc400 | Submit layer: builds the address and size arrays from recorded chunks |
| 0x11fd190–0x11fd911 | Constant engine (CCB) packet writers |
| 0x11fde00 | Deferred replay: re-runs recorded writer calls at fixed offsets of a pre-built command buffer |

No relocation and no RIP-relative reference points into 0x1072000–0x108c000, so every writer is reached by direct calls. Caller counts in this note are those direct calls.

## Command buffer object

Every writer takes a pointer to this object as its first argument. 0x1071270 initialises it from `(object, begin, size in bytes, callback, callback data)`; without a callback it installs one that returns false.

| Offset | Size | Contents |
|---|---|---|
| +0x00 | 8 | Begin of the buffer |
| +0x08 | 8 | End of the buffer |
| +0x10 | 8 | Write cursor |
| +0x18 | 8 | Reserve callback `bool (*)(object, dwords needed, data)` |
| +0x20 | 8 | Callback data |
| +0x28 | 4 | Header bits ORed into some packets: bit 0 = predicate (draws and dispatches), bit 1 = shader type (register writes, events) |
| +0x38 | 4 | Size in dwords |

A writer checks `(end - cursor) / 4` against the number of dwords it needs. If there is not enough room it calls the callback; on false it writes nothing. Then it writes the packet and advances the cursor. The submit layer's objects record one chunk per callback (records of 16 bytes from +0x58, count at +0x158), so one logical command buffer is submitted as several (address, size) pairs.

## PM4 packets

All packets are type 3 (`0xC0000000 | (count - 1) << 16 | opcode << 8 | bits`), except a single-dword filler `0x80000000` (type 2) that the NOP writer 0x10783c0 emits when asked for one dword. Register offsets in `SET_*_REG` packets are relative to a base per register space (dword indices): config 0x2000, SH 0x2C00 and context 0xA000 from the SI guide, and uconfig 0xC000. The SI guide has no uconfig space; its base follows from the game and the register reference: 0x1073fe0 writes a 6-bit value to uconfig offset 0x242, and the register reference puts `VGT_PRIMITIVE_TYPE` (field `PRIM_TYPE`, bits 5:0) at byte address 0x30908, dword 0xC242.

A header found by a byte scan counts only once a writer is found that stores it: the scan also matches bytes that span several instructions. For example, `0xC00B8A00` at 0x107255f is the end of a `movl $4, …`, a `mov (%rbx), %cl` and the first byte of a `shr`, inside a function that returns a size and writes no packet.

| Opcode | Name (unverified) | Example writer | Notes |
|---|---|---|---|
| 0x10 | `NOP` | 0x10783c0 | Padding, tags and embedded data (below) |
| 0x11 | `SET_BASE` | 0x10742f0 | Base for indirect draws |
| 0x12 | `CLEAR_STATE` | 0x10728d0 | Part of the default state block |
| 0x13 | `INDEX_BUFFER_SIZE` | 0x1075320 | |
| 0x15 | `DISPATCH_DIRECT` | 0x1074c50 | Shader-type bit set |
| 0x16 | `DISPATCH_INDIRECT` | 0x1074de0 | |
| 0x20 | `SET_PREDICATION` | 0x1077570 | |
| 0x24 | `DRAW_INDIRECT` | 0x10744d0 | |
| 0x25 | `DRAW_INDEX_INDIRECT` | 0x10746f0 | |
| 0x26 | `INDEX_BASE` | 0x10752c0 | |
| 0x27 | `DRAW_INDEX_2` | 0x1074360 | 0x10740a0 points it at indices inside the command buffer |
| 0x28 | `CONTEXT_CONTROL` | 0x10728d0 | `0x80000000, 0x80000000` |
| 0x2A | `INDEX_TYPE` | 0x1075270 | |
| 0x2C | `DRAW_INDIRECT_MULTI` *not in guide* | 0x10745d0 | No callers |
| 0x2D | `DRAW_INDEX_AUTO` | 0x10741b0 | |
| 0x2F | `NUM_INSTANCES` | 0x1075370 | |
| 0x33 | `INDIRECT_BUFFER_CONST` | 0x11fd850 | CCB side |
| 0x34 | `STRMOUT_BUFFER_UPDATE` | 0x1077d80 | |
| 0x35 | `DRAW_INDEX_OFFSET_2` | 0x1074420 | |
| 0x37 | `WRITE_DATA` | 0x1073b80 | |
| 0x38 | `DRAW_INDEX_INDIRECT_MULTI` *not in guide* | 0x10747f0 | No callers |
| 0x39 | `MEM_SEMAPHORE` | 0x1078740 | Name from the length and operands only (address, flags) |
| 0x3C | `WAIT_REG_MEM` | 0x1077b30 | Used before flips (0x26b0ce0) |
| 0x3F | `INDIRECT_BUFFER` | 0x1078670 | Compute ring: 0x108b330 |
| 0x40 | `COPY_DATA` | 0x1077e30 | |
| 0x42 | `PFP_SYNC_ME` | 0x1077be0 | |
| 0x46 | `EVENT_WRITE` | 0x1075da0 | Also the draw trailer (below) |
| 0x47 | `EVENT_WRITE_EOP` | 0x10778f0 | 24 callers: fences and labels |
| 0x48 | `EVENT_WRITE_EOS` | 0x1078320 | |
| 0x49 | `RELEASE_MEM` *not in guide* | 0x108b0f0 | Compute only |
| 0x50 | `DMA_DATA` *not in guide* | 0x1073e60 | |
| 0x58 | `ACQUIRE_MEM` *not in guide* | 0x1077f40 | 32 callers: cache actions |
| 0x59 | `REWIND` *not in guide* | 0x1078570 | Reserved space that is filled in later; 0x1078610 sets bit 31 of the dword after the header once the space is written |
| 0x68 | `SET_CONFIG_REG` | 0x1072990 | |
| 0x69 | `SET_CONTEXT_REG` | 0x1072970 | |
| 0x76 | `SET_SH_REG` | 0x10729b0 | User data (below) |
| 0x78 | `SET_QUEUE_REG` *not in guide* | 0x1089d10 | Compute only |
| 0x79 | `SET_UCONFIG_REG` *not in guide* | 0x10729e0 | |
| 0x81 | `WRITE_CONST_RAM` | 0x11fd3e0 | CCB |
| 0x83 | `DUMP_CONST_RAM` | 0x11fd280 | CCB |
| 0x84 | `INCREMENT_CE_COUNTER` | 0x11fd190 | CCB |
| 0x85 | `INCREMENT_DE_COUNTER` | 0x1078220 | DCB |
| 0x86 | `WAIT_ON_CE_COUNTER` | 0x10781d0 | DCB |
| 0x88 | `WAIT_ON_DE_COUNTER_DIFF` | 0x11fd230 | CCB |
| 0x8C | none | 0x1074a90 | 5-dword body: one dword of three bitfields (bits 0–9, 10–19, 20–30), the constant 0x420, a 40-bit address (low dword `& 0xFFFFFF00`, then the high byte), and the sixth argument shifted right by 8; 1 caller. Header has the predicate bit. The compute-side writer 0x108a590 writes only the bitfield dword (1-dword body) and no shader-type bit, then the 3-dword profiling slot (below) |
| 0x8D | none | 0x1074b50 | 3-dword body, followed by the draw trailer like a draw; 4 callers. The compute-side writer 0x108a680 writes a different 5-dword body (four argument dwords, then `(arg & 3) << 3 | 1`) with the shader-type bit set, after the profiling slot and before the draw trailer |

The default state block that the game writes itself at 0x10728d0 is `CONTEXT_CONTROL`, `CLEAR_STATE`, `NUM_INSTANCES` 1 and `CB_COLOR_CONTROL` = 0x00CC0010.

### Draw trailer

Every draw and dispatch writer ends with two more dwords. If the import behind 0x1089c40 returns true, they are `EVENT_WRITE` with event type 0x35; otherwise a `NOP` with an empty payload. The register reference's `VGT_EVENT_INITIATOR` list doesn't name event 53 (it goes from 52, `THREAD_TRACE_STOP`, to 54, `THREAD_TRACE_FLUSH`), so this event has no name here. The performance counter writers are guarded by the same call. With the import returning 0, the stream contains neither.

The compute-side 0x8C and 0x8D writers (0x108a590, 0x108a680) also reserve a 3-dword profiling slot under the same guard: `SET_UCONFIG_REG` of uconfig offset 0x342 without the shader-type bit (`0xC0017900, 0x342, value`), or else a `NOP` of the same length (`0xC0011000`). The 0x8C writer puts it after the packet, the 0x8D writer before.

### NOP tags

Several writers put a `NOP` whose first payload dword is `0x6875xxxx` before the packet they tag. The GPU skips them; they look like markers for capture tools. The parser can ignore them, but the tags tell what the next packet means:

| Tag | Writer | Followed by |
|---|---|---|
| `0x68750000 | align << 12` | 0x10760c0 (36 callers) | Nothing: the NOP payload is space the caller uses for data, aligned to `1 << align` |
| `0x68750004` | 0x10761a0 | `SET_SH_REG` of 4 user-data dwords (a 128-bit descriptor) |
| `0x68750005` | 0x1076240 | `SET_SH_REG` of 8 user-data dwords (a 256-bit descriptor) |
| `0x68750006` | 0x10762f0 | `SET_SH_REG` of 4 user-data dwords |
| `0x6875000d` | 0x10764a0 | `SET_SH_REG` of n user-data dwords copied from memory |
| `0x68750007`, `0x68750008`, `0x68750009`, `0x6875000a` | 0x1078930 and the other performance counter writers | Counter selection (`SET_UCONFIG_REG`, `DMA_DATA`) |
| `0x68750012` | 0x1078130 | Nothing; payload is one or two 64-bit values |
| `0x68750013` | 0x108b7f0 and two more (compute) | Nothing |
| `0x68752000` | 0x10740a0 | Nothing: the payload holds the indices that the `DRAW_INDEX_2` before it points to |
| `0x68750777` | 0x10784a0 | Nothing: 64 dwords reserved at the end of a DCB that goes to submit-and-flip (0x26afb30, 0x11fb3c0) |
| `0x68750778` | 0x1078500 | Nothing: 64 dwords with a 64-bit address and a 32-bit value; 1 caller |

So **command buffers hold data the GPU reads**: descriptors and constants in tag-0 payloads, and index data. Command buffer memory has to be GPU-readable like any other buffer.

## Constant engine

The submit calls take a CCB beside each DCB (address NULL and size 0 when a chunk has none). The state cache layer writes descriptors with `WRITE_CONST_RAM` (26 call sites), copies CE RAM to memory with `DUMP_CONST_RAM`, and counts with `INCREMENT_CE_COUNTER`; the DCB waits with `WAIT_ON_CE_COUNTER` and signals with `INCREMENT_DE_COUNTER`; the CCB throttles with `WAIT_ON_DE_COUNTER_DIFF`. The parser therefore has to run both streams of a submission and honour the counters, or run the CCB up to each `INCREMENT_CE_COUNTER` before the DCB passes the matching wait.

## Submission

### Graphics

| Import | Arguments | Use |
|---|---|---|
| `zwY0YV91TTI` | `(count, dcb addresses[], dcb sizes in bytes[], ccb addresses[], ccb sizes in bytes[])` | Submit. 12 call sites, for example 0x11faff0 and 0x11fb2c0 (which returns the result) and the replay at 0x26bb730 (which ignores it) |
| `iCO804ZgzdA` | Same as above | A second submit with the same arguments. At renderer start-up (0x216b1a2 → 0x26bb8e0) the game submits a one-dword DCB (the type-2 filler) through it and enables an optional feature only if the result is not `0x80d13fff`. Other callers: 0x11fb340, 0x11fc290 and two in 0x1094ad0/0x1096470 |
| `xbxNatawohc` | `(count, dcb addresses[], dcb sizes[], ccb addresses[], ccb sizes[], video-out handle, buffer index, flip mode, 64-bit flip argument)`, the last three on the stack | Submit and flip. 8 call sites; the frame end at 0x26b0ce0 and 0x26b0e30 passes NULL CCB arrays and 1 or 2 DCBs |
| `yvZ73uQUqrk` | `()` | Called right after every submit-and-flip at 0x26b0e04 and 0x26b0ee7; result ignored |

Before submit-and-flip, the game ends the DCB with the 64-dword `0x68750777` NOP (0x10784a0), presumably space the driver may overwrite. The flip itself is the import's job: the HLE presents the buffer through `libSceVideoOut`.

### Compute queues

The compute queue object (0x108c7b0–0x108ca00) holds two small integers at +0x0 and +0x4 (probably pipe and queue), the queue id at +0x8, the read-pointer address at +0x10 and a command buffer object over the ring at +0x18 (ring size in dwords at +0x50).

| Import | Reached through | Arguments → result | Use |
|---|---|---|---|
| `29oKvKXzEZo` | wrapper 0x1089a80, called by 0x108c7b0 (from 0x26b1130) | `(int, int, ring address, ring size in dwords, read-pointer address)` → queue id, negative on error | Map a compute queue on the game's ring. The wrapper stores the id and clears the read pointer |
| `bX5IbRvECXk` | thunk 0x1089b20, called by 0x108c830 (17 callers) | `(queue id, write offset in dwords)` | After 0x108c830 has written one `INDIRECT_BUFFER` (shader-type bit set) per command buffer into the ring: tells the driver how far the ring is written |
| `ArSg-TGinhk` | thunk 0x1089b10, called by 0x108c810 | `(queue id)` | Unmap the queue; the caller clears the id |
| `A+uGq+3KFtQ` | wrapper 0x1089ac0, no callers | As `29oKvKXzEZo` plus one stack argument | Unreachable variant of the map call |

So the HLE parses compute rings too: from the last read position to the reported write offset, following the `INDIRECT_BUFFER` packets, and writes the read pointer back.

## Shader binding

### User data

The user-data writers (0x1076390: 64-bit pointer, 155 call sites; 0x1076420: one dword; 0x10761a0, 0x1076240, 0x10762f0, 0x10764a0: tagged, above) take a stage index and map it to the first user-data register through the table at 0x48d15c0:

| Index | Stage | First user-data register |
|---|---|---|
| 0 | CS | 0x2E40 (`COMPUTE_USER_DATA_0`) |
| 1 | PS | 0x2C0C (`SPI_SHADER_USER_DATA_PS_0`) |
| 2 | VS | 0x2C4C |
| 3 | GS | 0x2C8C |
| 4 | ES | 0x2CCC |
| 5 | HS | 0x2D0C |
| 6 | LS | 0x2D4C |

Code paths exist for every stage (call sites pass every index). Whether the game runs GS, ES, HS or LS shaders at all is still for the shader footer (item 2) to show.

### Shader objects and the stage writers

The state cache layer binds a shader object per stage. Its layout, as that layer reads it:

| Offset | Contents |
|---|---|
| +0x0 | Bits 0–22: offset from the code start to an embedded constant block; bits 24–31: number of input-usage entries |
| +0x4 | u16: size of the embedded constant block in 16-byte units |
| +0x8 | Register block handed to the driver import. Its first two dwords are the program address (`(hi:lo) << 8`, as for `SPI_SHADER_PGM_LO/HI`) |
| stage-specific | Input-usage table, 4 bytes per entry: VS +0x28, PS +0x3c, ES +0x20, LS +0x20, GS +0x38, HS +0x40, CS +0x28 |

If the constant block is not empty, the layer builds a buffer descriptor for `code address + offset`, stride 16 (0x1070960), and binds it as slot 15 of the stage (0x108e200). So data follows the code in GPU memory at an offset the shader object records. Whether that is the `OrbShdr` footer or a separate block is for item 2.

The driver imports write the shader's registers. Each takes the cursor, a dword count and the register block, returns 0 on success, and the caller advances the cursor by the fixed count only then:

| Stage | Import | Dwords | Extra argument | Caller |
|---|---|---|---|---|
| VS | `gAhCn6UiU4Y` | 29 | u32 passed through by the caller | 0x108fbb0 |
| VS | `V31V01UiScY` | 29 | same | 0x108fbb0, used instead when three dwords of the new object (+0x18..+0x20) equal those of the bound one |
| PS | `bQVd5YzCal0` | 40 | none; NULL unbinds | 0x108fdd0 |
| PS | `4MgRw-bVNQU` | 40 | none | 0x108fdd0, used instead when eight dwords (+0x18..+0x34) equal the bound object's |
| CS | `KXltnCwEJHQ` | 25 | none | 0x1090020 (graphics ring), 0x1090ec0 (compute) |
| ES | `FUHG8sQ3R58` | 20 | u32 | 0x1090940 |
| GS | `UJwNuMBcUAk` | 29 | none | 0x1090b30, which then binds the GS object's copy shader as VS |
| GS | `nLM2i2+65hA` | 29 | none | 0x1090b30, used instead when three dwords of the new object equal the bound one's |
| HS | `VJNjFtqiF5w` | 30 | u32 read from a pointer the caller passes | 0x1090380, 0x1090590 |
| HS | `GNlx+y7xPdE` | 30 | same | same callers, used instead when the object's dwords and that u32 are unchanged |
| LS | `vckdzbQ46SI` | 23 | u32 | 0x10901c0, 0x1090590 |

The "used instead" variants have the same size; they probably avoid rewriting unchanged context registers. The HLE needs the meaning of each dword in the register blocks to write equivalent `SET_SH_REG`/`SET_CONTEXT_REG` packets and pad the rest with a `NOP`. The blocks come from the shader files, so the footer work (item 2) is where their layout gets pinned down; next to it, the HS/ES/VS writers that also set `VGT_GS_MODE` (0x10756b0), `VGT_GS_ONCHIP_CNTL` (0x1075750) and `VGT_ESGS_RING_ITEMSIZE` (0x1075a90) themselves show which registers the driver doesn't write.

## Fetch shaders

**Answer: the eboot generates them at run time.** 0x10877c0 writes a GCN program into a buffer:

1. `v_add_i32 v0, vcc, s<a>, v0` and `v_add_i32 v3, vcc, s<b>, v3` if the description names SGPRs for a vertex or instance offset;
2. one `s_load_dwordx4` per vertex buffer, from the table whose pointer SGPR is the input-usage entry of kind 0x17;
3. `s_waitcnt lgkmcnt(0)`;
4. one `buffer_load_format_x/xy/xyz/xyzw … idxen` per attribute, indexed by v0, v3, v1 or v2 depending on the attribute's step mode, `soffset` 0;
5. `s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)`, `s_setpc_b64 s[0:1]`, then one data dword.

The generator reads a description whose first u16 is the program size in bytes (written by 0x1087710). 0x1087450, 0x1087590 and 0x1087650 prepare that description; the game function 0x216c5d0 calls them, allocates the program from its own allocator with the size from the description and calls 0x10877c0 (at 0x216cab1). The GNM utility layer has 24 more call sites (0x1093a50–0x109454c, 0x11fa110–0x11faa92).

The address reaches the shader through user data: the VS binder 0x108fbb0 writes it with 0x1076390 into user-data dwords 0–1 of stage 2 (VS); the ES (0x1090940) and LS (0x10901c0, 0x1090590) binders do the same for their stages. The vertex shader calls it with `s_swappc_b64 s[0:1], s[0:1]`.

What this means for the translator:

- At draw time the fetch code is read from the address in the vertex stage's user-data dwords 0–1 and decoded up to its `s_setpc_b64`. Its code hash joins the cache key as the design says.
- The generator uses only SMRD, VOP2, SOPP, MUBUF and SOP1, which items 3a, 3b and 3d cover.
- Offline, real fetch shaders can be produced by calling 0x10877c0 itself through the differential harness, which maps the eboot. It calls no other game function, but it uses two libkernel imports for its stack protector: it loads the canary through the GOT slot 0x53e3be0 (`f7uOxY9mM1U`, `__stack_chk_guard`) and calls the PLT stub 0x2bbe4f8 (`Ou3iL1abvng`, `__stack_chk_fail`) at 0x1087ab4. The harness has to resolve both before the call. Its input comes from the shader objects, so this waits for item 2's register-block and input-usage layout.

**Shaders inside the eboot.** The eboot holds 5493 `OrbShdr` markers: 5139 in the executable segment between 0x2d0a04a and 0x46d6e92 and 354 in the data segment between 0x50dfb04 and 0x52e3da4, one `s_endpgm` per marker. 210 of them contain one `s_swappc_b64 s[0:1], s[0:1]` each. They carry no readable names near the marker. Item 2's eboot scan extracts them like the archive shaders.

## libSceGnmDriver imports

31 imports are reached through a thunk in 0x10899f0–0x1089c4f, either a plain `jmp` or a wrapper that reshapes the result. The other 19 have no thunk; a packet writer outside that range calls the PLT stub itself: `0H2vBYbTLHI` (0x10728b4), the 11 stage-register imports (0x1075400–0x1075a70, 0x108a8b0, 0x108a947), the 5 marker imports (0x10774c8, 0x107754f, 0x1078038, 0x10780bf, 0x1078119 and their compute twins at 0x108ae68–0x108b7d9), `1qXLHIpROPE` (0x1078489) and `nF6bFRUBRAU` (0x1089c94). The "Reached through" columns name the thunk or the writer. "Unreachable" means neither that function nor any function that calls it has a caller, and no relocation or RIP-relative reference points at them.

### Reachable

| NID | Reached through | Arguments → result | Role |
|---|---|---|---|
| `0H2vBYbTLHI` | 0x1072870 (13 callers) | `(cursor, dwords free)` → dwords written | Writes the driver's default graphics state into the DCB; the caller reserves 256 dwords and advances by the result |
| `nF6bFRUBRAU` | 0x1089c50 (5 callers, e.g. compute queue set-up 0x26b1130) | same | Same for a compute command buffer |
| `gAhCn6UiU4Y`, `V31V01UiScY` | 0x1075480, 0x10754f0 | see [shader binding](#shader-objects-and-the-stage-writers) | VS registers |
| `bQVd5YzCal0`, `4MgRw-bVNQU` | 0x10753c0, 0x1075420 | see above | PS registers |
| `KXltnCwEJHQ` | 0x1075840, 0x108a870 | see above | CS registers |
| `FUHG8sQ3R58` | 0x1075560 | see above | ES registers |
| `UJwNuMBcUAk`, `nLM2i2+65hA` | 0x10759d0, 0x1075a30 | see above | GS registers |
| `VJNjFtqiF5w`, `GNlx+y7xPdE` | 0x10755d0, 0x1075640 | see above | HS registers |
| `vckdzbQ46SI` | 0x10757d0 | see above | LS registers |
| `W1Etj-jlW7Y` | 0x1077fe0 (14), 0x108b6a0 | `(cursor, dwords, string)` → 0 on success | Debug marker with a string: the writer sizes it from `strlen` (2 + (len+8)/4 + 2·((len+12)/8) dwords). Opens a marker range |
| `7qZVNgEu+SY` | 0x10780e0 (14), 0x108b7a0 | `(cursor, 6)` | Closes a marker range: fixed 6 dwords, result ignored |
| `jiItzS6+22g` | 0x1077470, 0x108ae10 | `(cursor, dwords, string)` → 0 on success | Single debug marker with a string, sized like `W1Etj-jlW7Y` |
| `1qXLHIpROPE` | 0x1078440, called once by the replay 0x11fde00 | `(cursor, 7, u32, u32)`, result ignored | Writes 7 dwords from two integers. Not yet understood; 7 dwords is the size of a `WAIT_REG_MEM`, so it may be a wait on a video-out buffer |
| `zwY0YV91TTI` | thunk 0x1089a10 | see [submission](#graphics) | Submit DCB/CCB pairs |
| `iCO804ZgzdA` | thunk 0x1089a30 | see above | Second submit; `0x80d13fff` means unavailable to the start-up probe |
| `xbxNatawohc` | thunk 0x1089a20 | see above | Submit and flip |
| `yvZ73uQUqrk` | thunk 0x1089b50 | `()` | Called after each submit-and-flip; has a name in `symbols/nids.csv` |
| `29oKvKXzEZo` | wrapper 0x1089a80 | see [compute queues](#compute-queues) | Map compute queue |
| `bX5IbRvECXk` | thunk 0x1089b20 | see above | Report compute ring write offset |
| `ArSg-TGinhk` | thunk 0x1089b10 | see above | Unmap compute queue |
| `iBt3Oe00Kvc` | thunk 0x1089b30 (11 callers) | no arguments set up, result ignored | Called after an `mfence` and before setting the valid bit of a `REWIND` whose reserved space was just written (0x1078610, 0x1078630, 0x10786f0 and their CCB and compute twins), and in the destructors 0x33e910/0x33e9b0. Probably a CPU→GPU write-ordering or cache flush hint. A no-op returning 0 fits every caller |
| `jg33rEKLfVs` | wrapper 0x1089c40 → bool (139 callers) | `()` → int | "GPU profiling on": guards the event-0x35 trailer after every draw and dispatch (see [draw trailer](#draw-trailer)) and all performance counter programming. The HLE returns 0 |
| `26PM5Mzl8zc` | thunk 0x10899f0 (12 callers, all performance counter writers) | `(u32 0..1, u32 0..8)` → u8 | Maps a logical shader-engine/instance index to the value the performance counter writers put into uconfig register 0x200 (for example at 0x108281c; the same writer later writes 0xE0000000 there). The register reference doesn't list that register. Only reached when `jg33rEKLfVs` is true |
| `RU74kek-N0c` | thunk 0x1089a00 (2 callers, performance counter writers) | `(u32, u16 mask)` → u16 mask | Maps a logical instance mask to the hardware mask; same guard |
| `TLV4mswiZ4A` | wrapper 0x1089c30 → bool (0x21982f0, 0x21c1e60) | `()` → int | Not yet understood. The result is ORed into the game flag byte at 0x55406d1; no other RIP-relative reference to that byte was found. Returning 0 leaves the flag alone |
| `ln33zjBrfjk` | thunk 0x1089bc0 (0x26d3fb4, renderer start-up) | `()` → u64 | Returns the address the game asks for when it maps a 128 KiB direct-memory block with CPU and GPU read/write protection (`L-Q3LEjIbgA`, flags 0, alignment 64 KiB). Not yet understood which block it is; with flags 0 it is only a hint, so returning 0 lets the kernel choose |

### Unreachable

The HLE registers these as logging stubs.

| NID | Reached through | What the caller shows |
|---|---|---|
| `aj3L-iaFmyk` | wrappers 0x10774f0, 0x108ae90 | Like `jiItzS6+22g` with an extra u32 |
| `aPIZJTXC+cU` | wrappers 0x1078060, 0x108b720 | Like `W1Etj-jlW7Y` with an extra u32 |
| `A+uGq+3KFtQ` | 0x1089ac0 | Map-compute-queue variant (above) |
| `UoYY0DWMC0U` | 0x1089a60 | Returns a byte |
| `b08AgtPlHPg` | 0x1089b60 | Returns an int converted to bool |
| `b0xyllnVY-I`, `PVT+fuoS9gU`, `H7-fgvEutM0`, `qpGITzPE+Zc`, `gObODli-OH8`, `+xuDhxlWRPg`, `f85orjx7qts`, `HHo1BAljZO8`, `jtkqXpAOY6w`, `f33OrruQYbM`, `u9YKpRRHe-M`, `4UFagYlfuAM`, `xeTLfxVIQO4`, `9thMn+uB1is`, `d88anrgNoKY` | 0x1089a40–0x1089c20 | Plain `jmp`, nothing about the arguments. `d88anrgNoKY` has a name in `symbols/nids.csv` |

## Open questions and next steps

- **Register blocks of the stage writers.** Which register each dword of the blocks at shader object +0x8 holds. Next step: item 2 parses the footer; compare its values with the bitfields of `SPI_SHADER_PGM_RSRC1/2_*` and the context registers in the register guide. Until then the HLE can write `SPI_SHADER_PGM_LO/HI` and `RSRC1/2` from the first four dwords and fill the rest with a `NOP`.
- **Opcodes 0x8C, 0x8D.** No name yet. 0x8C/0x8D come in pairs with compute-side writers whose bodies differ from the graphics ones, and 0x8D sits where a draw sits; they may belong to a draw path that uses a compute command buffer. The header check above may name them. Next step: count them in captured streams once the game runs, and log each opcode once as the design says.
- **Packet names and field layouts.** Every opcode-to-name pairing in the packet table, and the field layouts of the packets the SI guide doesn't describe (`RELEASE_MEM`, `DMA_DATA`, `ACQUIRE_MEM`, `REWIND`, `SET_QUEUE_REG`, `SET_UCONFIG_REG`, the two `*_MULTI` draws), need checking against an AMD PM4 header (Linux `amdgpu`/`radeon` or Mesa) before the parser hard-codes them. The check is logged as a code-source row in references.md; the request goes to the research agent. The same check can name event type 0x35 and uconfig register 0x200.
- **`iCO804ZgzdA`'s start-up probe.** Find what the renderer flag at +0x121 (set at 0x216b1b3) switches before choosing the HLE result. Returning `0x80d13fff` keeps the feature off, which is the safer first choice.
- **`1qXLHIpROPE`, `TLV4mswiZ4A`, `ln33zjBrfjk`.** Roles above are partial. Each is called once or twice; a runtime log of their arguments once the renderer starts will settle them.
- **Reading the driver HLE code of an emulator** would answer the dword layouts directly. Since 2026-11-06 the source policy allows it.

## Second wave

Planned in [graphics.md](../design/graphics.md#command-buffers-to-the-first-frame-second-wave), items 15–20 and 25–27 (item 3 below). The list below is what this note contributed.

What the note gives the PM4 and submission issues:

1. **PM4 parser core** (`bbpm4`), after the packet names are checked (open questions): type-3 headers and the type-2 filler, the four register spaces and the predicate/shader-type bits, `NOP` skipping, `INDIRECT_BUFFER`/`INDIRECT_BUFFER_CONST`, `REWIND` (wait for the valid bit), and a log of unknown opcodes. Tests with synthetic streams.
2. **Constant engine**: CE RAM, `WRITE_CONST_RAM`/`DUMP_CONST_RAM`, the CE/DE counters, running a DCB and its CCB together.
3. **Draw and dispatch state**: the draw packets in the table, index state, user data, and the snapshot for the backend.
4. **`libSceGnmDriver` HLE, submission**: the three submits, the call after flip, default-state writers (standard packets from the AMD default, padded), markers (`NOP` with the string), the profiling queries returning 0, and logging stubs for the unreachable imports.
5. **Compute queues**: map/unmap, the ring parser and read-pointer write-back.
6. **Shader stage writers**: after item 2 has the register block layout.

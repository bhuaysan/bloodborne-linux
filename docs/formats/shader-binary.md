# Shader binaries (entries of the shader archives)

Each entry of the shader archives (see [bnd4.md](bnd4.md)) holds one or more shader programs for the PS4 GPU. This note covers how `tools/shaderbnd` splits an entry into programs (#114), and what it reads from the footers and the metadata at the end of the entry (#117).

## Sources and confidence

Worked out from the 18,517 entries of the 8 pinned shader archives with a hex dump and Python. No external format material was consulted. Field names are ours. What a field means was inferred from how its values line up with other data in the files (stated with each field); fields without such a check are listed as not interpreted, with the values seen.

The hardware stage of each program type was told from the program's code: which outputs it writes (`exp pos`/`param`, `exp mrt`, buffer stores, LDS writes) and which inputs it reads. The instruction classes come from the Sea Islands ISA guide (see `docs/references.md`).

The split was checked on all 20,218 programs: each program's code, taken from the start found below and the code length described under "Footer block", ends with `s_endpgm` (`00 00 81 BF`), and the program counts per archive match the table in `docs/design/graphics.md`.

## Entry layout

```
0x00         entry header, with the tag "Shdr" at 0x24
code_0       program 0: GCN code, footer data, footer block (0x1C bytes, starts with "OrbShdr")
             program 1: GCN code, footer data, footer block      (only in some entries)
             data rows: 16 bytes each, as many as the u32 at 0x38  (only in some entries)
meta         metadata: counts, records, name pool (to the end of the entry)
```

- **First program's code**: at 0x34 plus the 32-bit value at 0x3C. Confirmed by the `s_endpgm` check above.
- **Footer data**: between the end of a program's code and its footer block lie 0 to 152 bytes (0 in 2 programs) that are not GCN code. Not interpreted.
- **Footer block**: 0x1C bytes starting with the ASCII marker `OrbShdr`, see below. The next program's code starts right after the block.
- **Data rows**: in 418 entries, `16 × u32@0x38` bytes lie between the last footer block and the metadata (4 to 24 rows). The ones looked at hold float values such as 1.0 and 0.5. Not interpreted further.
- **Metadata**: at 0x24 plus the u32 at 0x10, see below.

### Entry header

All fields little-endian. Offsets are from the start of the entry.

| Offset | Size | Values | Meaning |
|---|---|---|---|
| 0x00 | 4 | `00 03 E2 20` in every entry | not interpreted |
| 0x04 | 4 | varies | **entry hash**: equal to the u32 at 0x10 of every footer block of the entry. How it is computed is unknown |
| 0x08 | 4 | 0 | not interpreted |
| 0x0C | 1 | 0–5 | **source stage**, matches the file extension in every entry: 0 `.vpo`, 1 `.ppo`, 2 `.cpo`, 3 `.gpo`, 4 `.hpo`, 5 `.dpo` |
| 0x0D | 1 | 1 | not interpreted |
| 0x0E | 2 | 0 | not interpreted |
| 0x10 | 4 | varies | **metadata offset**, from 0x24 |
| 0x14 | 4 | 0–2 | not interpreted. 1 in all ES and 2 in all LS vertex shaders, 0 in every other entry |
| 0x18 | 12 | varies | not interpreted. 0 in every `.vpo` and `.ppo` entry, nonzero in every `.cpo`, `.dpo`, `.gpo` and `.hpo` entry. In compute shaders it starts like three u16 thread counts (`20 00 01 00 01 00`, `00 01 01 00 01 00`); in hull shaders the last u32 is a float (1.0 to 16.0) |
| 0x24 | 4 | `Shdr` | tag |
| 0x28 | 4 | `07 00 01 00` | not interpreted (the 7 matches the footer version) |
| 0x2C | 1 | 1–7 | **hardware stage**: 1 VS (also the domain shaders), 2 PS, 3 GS, 4 CS, 5 ES, 6 LS, 7 HS. Agrees with the footer's program type in every entry |
| 0x2D | 1 | 0x0F–0x22 and others | not interpreted |
| 0x2E | 6 | 0 | not interpreted |
| 0x34 | 4 | varies | not interpreted |
| 0x38 | 4 | 0, 4–24 | **data rows** before the metadata |
| 0x3C | 4 | varies | **code offset** of the first program, from 0x34 |
| 0x40 | 4 | `FF FF FF FF` | not interpreted |
| 0x44 | to the code | varies | not interpreted. Probably register values for the program; not checked |

### Footer block

| Offset | Size | Values | Meaning |
|---|---|---|---|
| 0x00 | 7 | `OrbShdr` | marker |
| 0x07 | 1 | 7 | version |
| 0x08 | 4 | bits 0–1: 3; bits 2–5: 0–8; bits 6–7: 1; bits 8–31: varies | **program type** in bits 2–5 (table below) and **code size** in bytes in bits 8–31. The code size ends exactly after an `s_endpgm` in every program. Bits 0–1 and 6–7 are not interpreted |
| 0x0C | 4 | bytes 0–1 small, byte 2 12 (8 in 132 programs), byte 3 0 | not interpreted |
| 0x10 | 4 | varies | entry hash (see the entry header) |
| 0x14 | 4 | 0 | not interpreted |
| 0x18 | 4 | varies | not interpreted. Not a CRC-32, Adler-32 or SHA-256 prefix of the code; 4,312 distinct values over 20,218 programs |

Program types, and the hardware stage each one runs as. The stage follows from the code: VS-stage programs export positions and parameters, ES programs write their outputs with buffer stores and export nothing, LS programs write to LDS, GS programs read with buffer loads and write with buffer stores, and the copy shader reads with buffer loads and exports positions and parameters.

| Type | Extension | Stage | Programs | What it is |
|---|---|---|---|---|
| 0 | `.ppo` | PS | 3,209 | pixel shader |
| 1 | `.vpo` | VS | 10,116 | vertex shader |
| 2 | `.vpo` | ES | 1,699 | vertex shader in front of a geometry shader |
| 3 | `.vpo` | LS | 850 | vertex shader in front of a hull shader |
| 4 | `.cpo` | CS | 69 | compute shader |
| 5 | `.gpo` | GS | 1,701 | geometry shader (program 0) |
| 6 | `.gpo` | VS | 1,701 | its copy shader (program 1) |
| 7 | `.hpo` | HS | 404 | hull shader |
| 8 | `.dpo` | VS | 469 | domain shader |

So the game uses all of LS, HS, ES and GS. The two-program entries are exactly the 1,701 geometry shaders (1,694 in `gxflvershader.shaderbnd`, 7 in `gxffxshader.shaderbnd`), each followed by its copy shader. No domain shader runs as ES, so tessellation and geometry shaders are never combined.

The 20,218 programs have 3,832 distinct code hashes.

### Metadata

All offsets are from the start of the metadata. A name is a u32 offset, relative to the field that holds it, to an ASCII string in the name pool.

```
0x00  u32 resources      0x04  u32 0              0x08  u32 constants     0x0C  u32 samplers
0x10  u8  inputs         0x11  u8  outputs        0x12  u16 stream-out    0x14  u32 0
0x18  u32 name pool size
0x1C  records: resources, constants, samplers, inputs, outputs, stream-out, in this order
      name pool: NUL-terminated strings, up to the end of the entry
```

The records end exactly where the name pool starts in every entry.

| Record | Size | Fields |
|---|---|---|
| resource | 0x18 | u32 slot, u32 size (of a constant buffer, else 0), 4 kind bytes, u32 0, u32 0, name. Kinds seen: `16 04 6A 00` constant buffers (`cbParam`), `02 05 03 00` and `04 05 03 00` textures (`g_Texture0`, `g_IBLTex…`), `1A 06 00 00` and others; not interpreted further |
| constant | 0x24 | members of the constant buffers: 4 kind bytes, u32 offset, u32 size, three u32, name, and a second name (`(no_name)` or a struct name such as `PerInstanceData`). Not read by the tool |
| sampler | 0x10 | u32 slot, u32 0, u32 not interpreted (0–12), name (`SS_ClampLinear`) |
| input, output | 0x10 | 4 kind bytes (byte 2 is the semantic index), u32 0, name (`IN.tex0`), semantic (`TEXCOORD`, or `(no_name)`). Bytes 0, 1 and 3 are not interpreted |
| stream-out | 0x10 | only in 10 `_SO` geometry shaders. Not interpreted |

There is no separate entry point field. The entry point name shows up only as the name of the outputs: `PixelShader_Color` for a pixel shader's return value, `VertexShader_Tex1.position` for the members of a vertex shader's output struct. Compute shaders have no outputs, so their entry point name is not in the file.

## How the tool splits an entry

Program *k* starts at its code: `code_0` for the first one, the end of the previous footer block for the others. It ends where the next program starts, and the last one at the end of the entry. So each program file holds its code, its footer data and block and, for the last program, the data rows and the metadata too. The entry header belongs to no program; the tool keeps every entry as stored as well.

From the footer block the tool writes the program type, stage, code size and code hash (SHA-256 of the code bytes) into each program's row in `index.json`. From the metadata it writes the names, slots and kind bytes of the resources and samplers, and the names, semantics, semantic indexes and kind bytes of the inputs and outputs, once per entry under `metadata`. It refuses entries whose fields have values not listed here as seen (version, the fixed bits of the type field, zero fields, the entry hash), whose code size does not end after an `s_endpgm`, or whose metadata records don't line up with the name pool.

The marker is found by a byte search. In all 8 archives the number of markers equals the number of `s_endpgm`, so the search does not hit code by chance there.

## Programs embedded in the eboot

`shaderbnd.py --scan-eboot` counts the `OrbShdr` markers in `eboot.elf` from `tools/self2elf` and prints them by segment, stage and the bits 0–1 of the type field. It writes nothing. Of the 5,493 markers, 5,139 lie in the executable segment and 354 in the writable one:

| Segment | PS | VS | ES | LS | CS |
|---|---|---|---|---|---|
| executable | 5,028 | 37 | 37 | 37 | 0 |
| writable | 202 | 103 | 0 | 0 | 49 |

Every marker has version 7 and a program type from the table above. In 16 of the blocks in the writable segment, bits 0–1 of the type field are 1 instead of 3. The eboot has no GS, HS or domain shaders. Where these programs start, and whether they come with an entry header, is not worked out yet.

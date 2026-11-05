# Shader binaries (entries of the shader archives)

Each entry of the shader archives (see [bnd4.md](bnd4.md)) holds one or more shader programs for the PS4 GPU. This note covers what `tools/shaderbnd` needs to split an entry into programs. The footer contents are still open (#117).

## Sources and confidence

Worked out from the 18,517 entries of the 8 pinned shader archives with a hex dump and Python. No external format material was consulted. Field names are ours.

The split was checked on all 20,218 programs: each program's code, taken from the start found below and the code length described under "Footer block", ends with `s_endpgm` (`00 00 81 BF`), and the program counts per archive match the table in `docs/design/graphics.md`.

## Entry layout

```
0x00         entry header, with the tag "Shdr" at 0x24
code_0       program 0: GCN code, footer data, footer block (0x1C bytes, starts with "OrbShdr")
             program 1: GCN code, footer data, footer block      (only in some entries)
             data after the last footer block: names and other metadata
```

- The entry header starts with bytes `00 03 E2 20` in every entry, and has the tag `Shdr` at 0x24. The rest is not interpreted yet.
- **First program's code**: at 0x34 plus the 32-bit value at 0x3C. Confirmed by the `s_endpgm` check above.
- **Footer data**: between the end of a program's code and its footer block lie 0 to 152 bytes that are not GCN code. Not interpreted yet.
- **Footer block**: 0x1C bytes starting with the ASCII marker `OrbShdr`. The byte after the marker is 7 in every program. The next program's code starts right after the block.
- **Code length**: bits 8–31 of the 32-bit value at block offset 0x08. Used only for the check above; the tool doesn't rely on it.
- After the last block come the names the program uses (`g_Texture0`, `SS_ClampLinear`, `IN.tex0`, `TEXCOORD`, the entry point) and other data. Not interpreted yet.

Entries with two programs occur in `gxflvershader.shaderbnd` (1,694 entries) and `gxffxshader.shaderbnd` (7), and all of them are geometry shaders (`.gpo`). In the one looked at, the second program writes exports, as a copy shader would. That is not verified for the others.

## How the tool splits an entry

Program *k* starts at its code: `code_0` for the first one, the end of the previous footer block for the others. It ends where the next program starts, and the last one at the end of the entry. So each program file holds its code, its footer data and block and, for the last program, the trailing data too. The entry header belongs to no program; the tool keeps every entry as stored as well.

The marker is found by a byte search. In all 8 archives the number of markers equals the number of `s_endpgm`, so the search does not hit code by chance there.

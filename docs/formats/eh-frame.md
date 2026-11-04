# Unwind tables (.eh_frame, .eh_frame_hdr)

The PS4 ELFs carry standard x86-64 unwind tables, the ones C++ exceptions use. They matter for analysis because there is one FDE per function the linker kept, referenced or not, so they give a list of function starts. This page describes what the pinned files contain and how `tools/elf2std` exposes the tables to Ghidra.

## Sources and confidence

The layouts come from the LSB Core specification, chapter "Exception Frames" (`.eh_frame`, `.eh_frame_hdr`), and the pointer encodings and CIE/FDE contents from the DWARF specification, section "Call Frame Information" (both credited in `docs/references.md`). The numbers below were measured on the 8 pinned ELFs.

## Layout in the pinned files

The ELFs have no section headers (see [self.md](self.md)). The only way in is `PT_GNU_EH_FRAME`, which covers `.eh_frame_hdr`:

```
.eh_frame_hdr
  u8   version            1
  u8   eh_frame_ptr_enc   0x1b  pcrel | sdata4
  u8   fde_count_enc      0x03  udata4
  u8   table_enc          0x3b  datarel | sdata4
  s32  eh_frame_ptr       → start of .eh_frame
  u32  fde_count
  fde_count × { s32 initial_location, s32 fde_address }   relative to .eh_frame_hdr, sorted
```

`.eh_frame` has no size field anywhere. It is a sequence of records, each starting with a 32-bit length (0xffffffff means a 64-bit length follows), and ends with a zero length word. A record whose next word (the CIE id) is 0 is a CIE, otherwise it is an FDE and that word points back to its CIE.

Observed in all 8 files (**confirmed** by walking every record):
- `.eh_frame` sits in the executable `PT_LOAD`, directly followed by `.eh_frame_hdr`, which runs to the end of that segment.
- `.eh_frame` ends with a zero terminator immediately before `.eh_frame_hdr`.
- Every CIE has augmentation `zR`, or `zPLR` (personality routine and LSDA, eboot and libc only), and FDE pointer encoding 0x1b (pcrel | sdata4).

| File | CIEs | FDEs in `.eh_frame` | FDEs in the search table |
|---|---|---|---|
| eboot | 4,390 | 208,952 | 162,959 |
| libc | 907 | 2,682 | 2,164 |
| libSceFace | 97 | 673 | 673 |
| libSceFios2 | 51 | 679 | 650 |
| libSceHand | 8 | 32 | 32 |
| libSceHeadTracker | 70 | 605 | 605 |
| libSceS3DConversion | 38 | 1,228 | 1,228 |
| libSceSmart | 172 | 2,359 | 1,329 |

**Dead FDEs.** In all 8 files, the FDEs missing from the search table are exactly the ones with a raw `pc_begin` of 0 (45,993 in the eboot), so with pcrel encoding they "start" at their own address inside `.eh_frame`. They look like FDEs of functions the linker discarded, left in place with a zeroed relocation. Their `pc_range` is still the old function size. `PT_GNU_EH_FRAME` is sized for a table of all FDEs (in every file 12 + 8 × FDEs in `.eh_frame` bytes) but `fde_count` counts only the live ones, so the end of the table is unused.

## What elf2std adds

`elf2std` finds `.eh_frame_hdr` through `PT_GNU_EH_FRAME`, decodes `eh_frame_ptr` (absptr, pcrel or datarel; udata4/sdata4/udata8/sdata8) and walks the records from there. The walk stops after the zero terminator. Without one it stops at the end of the containing segment's file data, or at the start of `.eh_frame_hdr` when that follows in the same segment. A record that crosses that limit is an error. It then writes a section header table at the end of the file (not loaded):

| Section | Type | Flags | Covers |
|---|---|---|---|
| (null) | | | |
| `.text` | PROGBITS | AX | the executable segment around the two unwind sections |
| `.eh_frame` | PROGBITS | A | from `eh_frame_ptr` to the terminator, inclusive |
| `.eh_frame_hdr` | PROGBITS | A | `PT_GNU_EH_FRAME` |
| `.data` | PROGBITS | WA | the file data of the writable segment |
| `.rodata` | PROGBITS | A | the file data of an original segment that is neither executable nor writable (none of the pinned files has one) |
| `.bss` | NOBITS | WA | the zero-filled tail (`p_memsz` > `p_filesz`) of an original segment; flags follow that segment (WA in all pinned files) |
| `.dynstr` | STRTAB | A | the string table in the new segment |
| `.dynamic` | DYNAMIC | A | the new dynamic table, `sh_link` → `.dynstr` |
| `.shstrtab` | STRTAB | | section names |

The sections are sorted by address. Files without `PT_GNU_EH_FRAME` get no section headers at all.

Why the extra sections:
- **`.dynamic`, `.dynstr`**: once section headers exist, readelf looks up the dynamic table through its section and reports an error without one, and the dynamic section's `sh_link` has to name a string table.
- **`.text`, `.data`, `.rodata`, `.bss`**: once section headers exist, Ghidra builds its memory blocks from them. Segment parts not covered by a section still get loaded, but without the execute permission and without the zero-filled tail. Covering every loaded byte keeps the blocks as they were. The names are generic: `.text` also holds the PLT and read-only data, and `.data` the GOT.
- **Nothing else in the new segment**: it is read-only and fully backed by the file, so its uncovered parts load correctly. A section over the symbol and relocation tables there (tried as `.rodata`) made Ghidra stop turning PLT stubs into thunks of the imports.

The names `.text`, `.data`, `.rodata` and `.bss` are used only for these filler sections. No `.dynsym`, `.rela.*` or `.hash` sections are written. Ghidra finds those tables through the dynamic table, as before. readelf, once section headers exist, lists relocations and dynamic symbols only from sections, so `readelf -r` and `readelf --dyn-syms` now need `--use-dynamic`.

## Result in Ghidra 12.1.4

Ghidra's exception handler analyzer finds the two sections by name, marks them up as data and creates a function at every FDE in the search table. On the dead FDEs it logs "Failed to disassemble" errors (282 in libc) but creates no functions, instructions or bookmarks inside `.eh_frame`.

Full auto-analysis of the eboot, before (no section headers) and after:

| | Before | After |
|---|---|---|
| Functions | 99,715 | 167,056 |
| Thunks / thunks to imports / external functions | 1,596 / 1,350 / 681 | 6,097 / 1,393 / 681 |
| FDEs (search table) without a function at their start | 67,324 | 0 |
| FDEs without any decoded instruction | 20,950 | 0 |
| Bytes covered by FDEs that are decoded | 95.94% | 98.52% |
| Error bookmarks (all "Bad Instruction") | 9 | 26 |
| Analysis time | 1,525 s | 1,723 s |

None of the 45,993 dead FDE addresses gets a function or an instruction. Ghidra logs 12 "Failed to disassemble" errors for them, without bookmarks.

**Error bookmarks.** 5 of the old 9 are gone, because function boundaries from the FDEs stop some wrong flows. The 22 new ones are of a kind that already existed: almost all follow a call to a PLT stub of an import that never returns (mostly `Ou3iL1abvng#p#J` and `lKEN2IebgJ0#q#q`). Ghidra doesn't know these imports don't return, so it decodes the bytes after the call, typically a jump table, and fails. The code is newly reached only because the functions around it now exist. Marking those imports as non-returning in Ghidra should remove them. That is Ghidra configuration, not something the ELF can express.

Memory blocks: the executable segment `segment_0` (r-x) becomes `.text` (r-x), `.eh_frame` (r--) and `.eh_frame_hdr` (r--). The writable segment `segment_1` becomes `.data` and `.bss` (rw-, `.bss` uninitialized). The new segment keeps its uncovered parts as `segment_N.1`/`segment_N.3` around `.dynstr` and `.dynamic`. A `.shstrtab` and an `_elfSectionHeaders` overlay are added. Addresses and the non-loaded Sony overlays are unchanged.

In libc the only change in imports is that the `DT_PREINIT_ARRAY` function at 0x5b0, which Ghidra used to show as a thunk to a made-up external `_PREINIT_0`, is now an ordinary function.

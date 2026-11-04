# PS4 ELF dynamic linking data

How a PS4 ELF describes its imports, exports and relocations, and how `tools/elf2std/elf2std.py` rewrites it into a standard ELF that stock tools understand. The ELF itself is rebuilt from the SELF container first (see [self.md](self.md)).

## Sources and confidence

Everything here was worked out from the 8 ELFs of the pinned build (the eboot and 7 bundled PRXs). No public documentation of the Sony dynamic tags was found in allowed sources. The tag names below are our own descriptive names. The standard ELF parts (tag numbers, `Elf64_Sym`, `Elf64_Rela`, the SysV hash table) follow the System V ABI (credited in `docs/references.md`).

The main checks:
- Every table found through a tag parses cleanly in all 8 files: string offsets stay inside the string table, the symbol table size is a multiple of 24, and relocation counts add up.
- In all 8 files, every `JUMP_SLOT` relocation (1,007 in total) targets a GOT slot whose initial value points 6 bytes into a PLT stub, and that stub is a `jmp [rip+x]` through the same slot.
- readelf reads the converted files without warnings, and Ghidra labels every PLT stub with its import.

Confidence:
- **confirmed**: the table at that offset parses and is consistent in all 8 files
- **observed**: present in all 8 files, meaning not verified
- **unknown**: not interpreted

## Where the data lives

```
PT_SCE_DYNLIBDATA (0x61000000)   not loaded, p_vaddr = 0, p_memsz = 0
  ├─ Sony hash table
  ├─ string table
  ├─ symbol table        Elf64_Sym, 24 bytes each
  ├─ PLT relocations     Elf64_Rela, 24 bytes each
  ├─ other relocations   Elf64_Rela
  └─ dynamic table       ← PT_DYNAMIC points here (file offset only, p_vaddr = 0)
```

The order above is what the files contain, but the tools don't depend on it. Only the offsets in the dynamic table are used.

On Linux the same tables sit in loaded segments and the dynamic table refers to them by virtual address. On the PS4 the dynamic table refers to them by **offset into the dynlibdata segment**, using tags in the `0x61000000` range. The tables themselves have the standard layouts. Because the segment isn't loaded, the system loader must read it from the file.

## Dynamic tags

Standard tags that appear: `DT_NEEDED` (1), `DT_INIT` (0xC), `DT_FINI` (0xD), `DT_SONAME` (0xE, PRX only), `DT_DEBUG` (0x15), `DT_TEXTREL` (0x16, eboot only), `DT_INIT_ARRAY`/`DT_FINI_ARRAY` and their sizes (0x19–0x1C), `DT_FLAGS` (0x1E), `DT_PREINIT_ARRAY` and its size (0x20, 0x21). They hold string table offsets or virtual addresses, as on Linux.

Sony tags. Offset means an offset into the dynlibdata segment.

| Tag | Our name | Value | Standard equivalent | Confidence |
|---|---|---|---|---|
| 0x61000025 | DT_SCE_HASH | offset | DT_HASH, but see below | confirmed (layout) |
| 0x61000027 | DT_SCE_PLTGOT | virtual address | DT_PLTGOT | confirmed |
| 0x61000029 | DT_SCE_JMPREL | offset | DT_JMPREL | confirmed |
| 0x6100002B | DT_SCE_PLTREL | 7 (`DT_RELA`) | DT_PLTREL | confirmed |
| 0x6100002D | DT_SCE_PLTRELSZ | size | DT_PLTRELSZ | confirmed |
| 0x6100002F | DT_SCE_RELA | offset | DT_RELA | confirmed |
| 0x61000031 | DT_SCE_RELASZ | size | DT_RELASZ | confirmed |
| 0x61000033 | DT_SCE_RELAENT | 24 | DT_RELAENT | confirmed |
| 0x61000035 | DT_SCE_STRTAB | offset | DT_STRTAB | confirmed |
| 0x61000037 | DT_SCE_STRSZ | size | DT_STRSZ | confirmed |
| 0x61000039 | DT_SCE_SYMTAB | offset | DT_SYMTAB | confirmed |
| 0x6100003B | DT_SCE_SYMENT | 24 | DT_SYMENT | confirmed |
| 0x6100003D | DT_SCE_HASHSZ | size of the hash table | none | confirmed |
| 0x6100003F | DT_SCE_SYMTABSZ | size of the symbol table | none (Linux derives the count from the hash table) | confirmed |
| 0x61000007 | | 0 here | | unknown |
| 0x61000009 | | string table offset (in libSceHand, a build path) | | observed |
| 0x6100000D | | packed: string offset + ids (this module) | | observed |
| 0x6100000F | | packed: string offset + ids (a needed module), one per module | | observed |
| 0x61000011 | | 0 here | | unknown |
| 0x61000013 | | packed: string offset + ids (an exported library), PRX only | | observed |
| 0x61000015 | | packed: string offset + ids (an imported library), one per library | | observed |
| 0x61000017 | | packed ids, pairs with 0x13 | | observed |
| 0x61000019 | | packed ids, pairs with 0x15 | | observed |

The packed values keep a string table offset in the low 32 bits. The upper 32 bits hold small numbers that look like an id and a version. Decoding these module and library records is the subject of the import listing task, not of this note.

### Sony hash table

The table at `DT_SCE_HASH` has the SysV layout: `nbucket`, `nchain`, then the buckets and chains as 32-bit words. `nchain` equals the symbol count and the size matches `DT_SCE_HASHSZ` in all 8 files. The **hash function is not the SysV ELF hash**: looking up each symbol with the SysV function, applied either to the whole name or to the part before the first `#`, finds only about as many symbols as chance would. The real function is unknown.

## Symbols

Symbol table entries are standard `Elf64_Sym`. Imported symbols are undefined (`st_shndx` = 0) and are named `<NID>#<library>#<module>`, for example `8zTFvBIAIN8#B#C`:
- the NID is 11 characters from a base64-style alphabet (letters, digits, `+`, `-`),
- the library and module parts are short ids. They refer to the library and module records in the dynamic table.

Exported symbols use the same form and point at their definition (`st_value`). Their library and module ids name the file's own library. A few symbols keep plain names (`module_start`, `module_stop`).

## Relocations

All relocation types are standard x86-64:

| Type | Name | Where |
|---|---|---|
| 1 | R_X86_64_64 | data pointers to symbols. Those against imported data use `symbol + 0x10`, which looks like C++ type-info objects pointing into their class vtable |
| 6 | R_X86_64_GLOB_DAT | GOT entries for data symbols |
| 7 | R_X86_64_JUMP_SLOT | PLT GOT entries, one per imported function |
| 8 | R_X86_64_RELATIVE | base-relative pointers, the bulk (about 231,000 in the eboot) |
| 16 | R_X86_64_DTPMOD64 | TLS module id (libc and libSceFios2 only) |

The PLT is a standard lazy-binding PLT: `PLT0` pushes `GOT[1]` and jumps through `GOT[2]`, and each 16-byte entry jumps through its GOT slot. The GOT slot initially points back into its own stub, at the `push index`, so the first call goes through the resolver.

## Converting to a standard ELF

`elf2std` makes these changes and keeps every other byte:

1. **`e_type` becomes `ET_EXEC` (2).** The image is position-independent and `ET_DYN` would be the honest type, but Ghidra rebases `ET_DYN` images to 0x100000. `ET_EXEC` keeps the link addresses, which `symbols/` uses.
2. **A new `PT_LOAD` segment** (read-only) at the first 0x4000-aligned address above the image. Its file data is appended to the end of the file. It holds:
   - a copy of the dynlibdata segment, so `vaddr = new base + offset` works for every Sony offset,
   - a new SysV hash table over the same symbols (a copy of the Sony table would make standard lookups fail),
   - a new dynamic table with standard tags.
3. **`PT_DYNAMIC`** points at the new dynamic table.
4. **The new program header** goes into the zero-filled gap after the existing ones. There is room up to file offset 0x4000.

The new dynamic table contains the standard tags of the original table unchanged, plus `DT_HASH`, `DT_STRTAB`, `DT_SYMTAB`, `DT_JMPREL`, `DT_RELA` (virtual addresses in the new segment), and `DT_PLTGOT`, `DT_PLTREL`, `DT_PLTRELSZ`, `DT_RELASZ`, `DT_RELAENT`, `DT_STRSZ`, `DT_SYMENT` (values copied). The other Sony tags are dropped from it. They stay readable in the copied original table.

### Result in Ghidra 12.1.4

- The code segment loads at its link address (image base 0).
- Every PLT stub becomes a thunk named after its import, pointing to an external function with the encoded name.
- Exported functions are labeled at their definitions. `DT_INIT` and `DT_FINI` are labeled.
- The import log shows warnings only for `R_X86_64_64` relocations against external data plus an offset. These can't be resolved without the target library and are expected.
- The non-loaded Sony segments still show up as separate overlay blocks. They are harmless.

## Open questions

- The Sony hash function.
- Tags 0x61000007 and 0x61000011 (0 in every file).
- The exact packing of the module and library records.

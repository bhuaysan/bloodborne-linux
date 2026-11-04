# SELF (signed ELF container)

The PS4 doesn't load plain ELF files. The executable (`eboot.bin`) and the bundled modules (`sce_module/*.prx`) are SELF files: a container holding a regular 64-bit ELF header and program headers, the segment data, and signing metadata.

The files in this game build are **fake-signed SELFs**: no segment is encrypted or compressed, and the signature and digest areas are zero. So the ELF can be rebuilt by copying bytes. `tools/self2elf/self2elf.py` does this.

## How this note was made

Everything here comes from the 8 SELF files of the pinned build (see `docs/clean-room.md`). No external document was read. The main check is that the 32-byte digest in the extended info block equals SHA-256 of the rebuilt ELF for all 8 files. That confirms the segment mapping, the trailer rule and the location of the digest. Field names are our own and descriptive.

Confidence:
- **confirmed**: checked by the digest match or by parsing all 8 files
- **observed**: same value in all 8 files, meaning not verified
- **unknown**: not interpreted

## Layout

All values are little-endian.

```
0x000  SELF header              0x20 bytes
0x020  segment entries          num_entries * 0x20
       ELF header               0x40
       program headers          e_phnum * 0x38
       (pad to 16)
       extended info            0x40
       control block            0x30
header_size:
       meta area                meta_size bytes
header_size + meta_size:
       segment data             as given by the entries
file_size:
       trailer                  SCE_LIBVERSION bytes (optional)
```

### SELF header (0x20 bytes)

| Off | Size | Field | Value in this build | Confidence |
|---|---|---|---|---|
| 0x00 | 4 | magic | `4F 15 3D 1D` | confirmed |
| 0x04 | 1 | version | 0 | observed |
| 0x05 | 1 | unknown | 1 | unknown |
| 0x06 | 1 | endianness (1 = little) | 1 | observed |
| 0x07 | 1 | unknown | 0x12 | unknown |
| 0x08 | 4 | unknown | 0x101 | unknown |
| 0x0C | 2 | header_size: offset of the meta area | 0x3D0 to 0x400 | confirmed |
| 0x0E | 2 | meta_size | 0x3D0 | confirmed |
| 0x10 | 8 | file_size: end of the signed data (segment data ends here) | | confirmed |
| 0x18 | 2 | num_entries | 6 or 8 | confirmed |
| 0x1A | 2 | unknown flags | 0x22 | unknown |
| 0x1C | 4 | padding | 0 | observed |

### Segment entry (0x20 bytes)

| Off | Size | Field | Confidence |
|---|---|---|---|
| 0x00 | 8 | properties (bits below) | partly confirmed |
| 0x08 | 8 | offset of the data in the SELF | confirmed |
| 0x10 | 8 | stored size | confirmed |
| 0x18 | 8 | size after unpacking (always equal to the stored size here) | observed |

Property bits:

| Bits | Meaning | Confidence |
|---|---|---|
| 1 | encrypted (never set here; the tool refuses it) | unknown |
| 2 | set on every entry, probably "signed" | observed |
| 3 | compressed (never set here; the tool refuses it) | unknown |
| 11 | set on data entries | observed |
| 12–15 | block size as `1 << (12 + n)`. Data entries have n = 2, so 16 KiB | observed |
| 16 | the entry is a **block digest table**, not ELF data | confirmed |
| 20–31 | target index (see below) | confirmed |

Two kinds of entries alternate:

- **Data entry** (bit 16 clear, `0x2804` in the low bits): bits 20–31 are a program header index. The entry's bytes go into the ELF at that header's `p_offset`, and the size equals its `p_filesz`.
- **Block digest table** (bit 16 set, `0x10004` in the low bits): bits 20–31 are the index of another *segment entry*, the data entry that follows it. The table holds 32 bytes per 16 KiB block of that entry, which fits a SHA-256 per block. In these fake-signed files the tables are all zero.

Only some program headers have their own data entry: the two `PT_LOAD`s, `PT_SCE_DYNLIBDATA` and, where present, `PT_SCE_COMMENT` (eboot and libc only). The others (`PT_INTERP`, `PT_TLS`, `PT_GNU_EH_FRAME`, `PT_SCE_PROCPARAM`/`MODULEPARAM`, `PT_DYNAMIC`) lie inside one of those, so they need no data of their own.

### Trailer: SCE_LIBVERSION

`PT_SCE_LIBVERSION` (0x6FFFFF01) has no segment entry. Its bytes are the last bytes of the file, after `file_size`. The trailer length equals that header's `p_filesz` in all 8 files (confirmed by the digest).

### Extended info (0x40 bytes)

Starts at the end of the program headers, rounded up to 16.

| Off | Size | Field | Confidence |
|---|---|---|---|
| 0x00 | 8 | an ID: `0x3100000000000001` for the eboot, `…02` for the PRXs | observed |
| 0x08 | 8 | 1 in all files | observed |
| 0x10 | 8 | 0 | observed |
| 0x18 | 8 | 0 | observed |
| 0x20 | 32 | **SHA-256 of the complete rebuilt ELF** | confirmed |

The 0x30-byte block after it starts with a 16-bit value 3. The rest is zero. Meaning unknown.

### Meta area

`meta_size` bytes at `header_size`. In these files it is zero except for one 32-bit value. This is where keys and the signature would sit in a real signed SELF. The tool ignores it.

## The embedded ELF

The ELF header and program headers are copied verbatim. Things that differ from a Linux ELF:

- `EI_OSABI` = 9 (FreeBSD). The PS4 OS is derived from FreeBSD.
- `e_type` is in the OS-specific range: 0xFE10 for the eboot, 0xFE18 for the PRXs. The names "dynamic executable" / "dynamic library" follow from which file is which.
- No section headers (`e_shoff` = 0, `e_shnum` = 0). readelf shows only the program headers.
- The eboot's virtual addresses start at 0, so the executable is position-independent and the loader picks the base.
- The eboot's entry point is 0xA0.

Program header types seen:

| `p_type` | Our name | Notes |
|---|---|---|
| 0x1 | PT_LOAD | Code (R E) and data (RW). Data has `p_memsz` > `p_filesz` (bss). |
| 0x2 | PT_DYNAMIC | `p_vaddr` = 0. The file data lies inside the DYNLIBDATA segment, which isn't loaded. |
| 0x3 | PT_INTERP | `/libexec/ld-elf.so.1`, at the start of the code segment |
| 0x7 | PT_TLS | |
| 0x6474E550 | PT_GNU_EH_FRAME | |
| 0x61000000 | PT_SCE_DYNLIBDATA | Not loaded (`p_memsz` = 0). Holds dynamic linking data. Imports and NIDs are in here (issue for M1). |
| 0x61000001 | PT_SCE_PROCPARAM | eboot only, at the start of the data segment |
| 0x61000002 | PT_SCE_MODULEPARAM | PRX only, same place |
| 0x6FFFFF00 | PT_SCE_COMMENT | Not loaded. eboot and libc only. |
| 0x6FFFFF01 | PT_SCE_LIBVERSION | Not loaded. Stored as the file trailer. |

The `PT_SCE_*` names are our own descriptive names for these values.

## Rebuild algorithm

1. Check the file's SHA-256 against the pinned build.
2. Parse the header, the entries and the ELF header + program headers.
3. Allocate `max(p_offset + p_filesz)` zero bytes. Copy the ELF header and program headers to offset 0.
4. For each data entry, copy its bytes to `phdr[index].p_offset`.
5. Copy the trailer (bytes after `file_size`) to the `PT_SCE_LIBVERSION` offset.
6. Check that every program header with `p_filesz` > 0 is covered.
7. Compare SHA-256 of the result with the extended info digest.

The gaps between segments (for example from the end of the program headers to 0x4000) are zero-filled. The digest match shows they are zero in the original ELF too.

## Open questions

- Header bytes 0x05, 0x07, 0x08 and 0x1A, and the extended info fields 0x00–0x18.
- The contents of the 0x30-byte control block and the meta area in a real signed SELF.
- Bits 2 and 11 of the segment properties.
- The external format documentation (PS4 developer wiki) couldn't be fetched (HTTP 403). Field names and the unknowns could be checked against it later and credited in `docs/references.md`.

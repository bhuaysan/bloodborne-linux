# SELF (signed ELF container)

The PS4 doesn't load plain ELF files. The executable (`eboot.bin`) and the bundled modules (`sce_module/*.prx`) are SELF files: a container holding a regular 64-bit ELF header and program headers, the segment data, and signing metadata.

The files in this game build are **fake-signed SELFs**: no segment is encrypted or compressed, and the signature and digest areas are zero. So the ELF can be rebuilt by copying bytes. `tools/self2elf/self2elf.py` does this.

## Sources and confidence

The layout was first worked out from the 8 SELF files of the pinned build (see `docs/clean-room.md`). It was then compared with the PS4 Developer Wiki page on SELF (credited in `docs/references.md`). The wiki supplied most field names. Several findings here aren't on the wiki page: the block digest table entries, the SCE_LIBVERSION trailer, the meta size formula, and the 0x30-byte block.

The main check is that the digest in the program identification header equals SHA-256 of the rebuilt ELF for all 8 files. That confirms the segment mapping, the trailer rule and the location of the digest.

Confidence:
- **confirmed**: checked by the digest match or by parsing all 8 files
- **wiki**: name or meaning from the wiki, consistent with our files
- **observed**: same value in all 8 files, meaning not verified
- **unknown**: not interpreted

## Layout

All values are little-endian.

```
0x000  SELF header                      0x20 bytes
0x020  segment entries                  num_segments * 0x20
       ELF header                       0x40
       program headers                  e_phnum * 0x38
       (pad to 16)
       program identification header    0x40
       unknown block                    0x30
header_size:
       meta area                        meta_size bytes
header_size + meta_size:
       segment data                     as given by the entries
file_size:
       trailer                          SCE_LIBVERSION bytes (optional)
```

### SELF header (0x20 bytes)

| Off | Size | Field | Value in this build | Confidence |
|---|---|---|---|---|
| 0x00 | 4 | magic | `4F 15 3D 1D` | confirmed |
| 0x04 | 4 | unknown, constant | `00 01 01 12` | wiki: "always" this value |
| 0x08 | 4 | program type | 0x101 | wiki |
| 0x0C | 2 | header_size: offset of the meta area | 0x350, 0x3D0 or 0x400 | confirmed |
| 0x0E | 2 | meta_size (wiki: "signature size") | 0x330 or 0x3D0 | confirmed |
| 0x10 | 4 | file_size: end of the signed data (segment data ends here) | | confirmed |
| 0x14 | 4 | padding | 0 | wiki |
| 0x18 | 2 | num_segments | 6 or 8 | confirmed |
| 0x1A | 2 | unknown | 0x22 | wiki: "always" this value |
| 0x1C | 4 | padding | 0 | wiki |

The tool checks the magic and the constant at 0x04.

### Segment entry (0x20 bytes)

| Off | Size | Field | Confidence |
|---|---|---|---|
| 0x00 | 8 | flags (bits below) | partly confirmed |
| 0x08 | 8 | offset of the data in the SELF | confirmed |
| 0x10 | 8 | size as stored (encrypted/compressed) | confirmed |
| 0x18 | 8 | size after decryption/decompression (equal here) | wiki, observed |

Flag bits:

| Bit(s) | Meaning | In this build | Confidence |
|---|---|---|---|
| 0 (0x1) | ordered | never set | wiki ("ordered?") |
| 1 (0x2) | encrypted. The tool refuses it. | never set | wiki |
| 2 (0x4) | signed | always set | wiki |
| 3 (0x8) | compressed (deflate). The tool refuses it. | never set | wiki |
| 11 (0x800) | blocked: data is split into blocks | data entries | wiki |
| 12–15 | block size as `1 << (12 + n)`. Data entries have n = 2, so 16 KiB. | data entries | observed |
| 16 (0x10000) | the entry is a **block digest table**, not ELF data | table entries | confirmed |
| 20–31 | ID: target index (see below) | | wiki (name), confirmed (meaning) |

Two kinds of entries alternate:

- **Data entry** (bit 16 clear, `0x2804` in the low bits): the ID is a program header index. The entry's bytes go into the ELF at that header's `p_offset`, and the size equals its `p_filesz`.
- **Block digest table** (bit 16 set, `0x10004` in the low bits): the ID is the index of another *segment entry*, the data entry that follows it. The table holds 32 bytes per 16 KiB block of that entry, which fits a SHA-256 or HMAC-SHA256 per block. In these fake-signed files the tables are all zero.

Only some program headers have their own data entry: the two `PT_LOAD`s, `PT_SCE_DYNLIBDATA` and, where present, `PT_SCE_COMMENT` (eboot and libc only). The others (`PT_INTERP`, `PT_TLS`, `PT_GNU_EH_FRAME`, `PT_SCE_PROCPARAM`/`MODULEPARAM`, `PT_DYNAMIC`) lie inside one of those, so they need no data of their own.

### Trailer: SCE_LIBVERSION

`PT_SCE_LIBVERSION` (0x6FFFFF01) has no segment entry. Its bytes are the last bytes of the file, after `file_size`. The trailer length equals that header's `p_filesz` in all 8 files (confirmed by the digest). The wiki only guesses that "some extra data" follows the signed part.

### Program identification header (0x40 bytes)

Starts at the end of the program headers, rounded up to 16. Field names are from the wiki.

| Off | Size | Field | Value in this build | Confidence |
|---|---|---|---|---|
| 0x00 | 8 | program authority ID | `0x3100000000000001` (eboot), `…02` (PRX) | wiki |
| 0x08 | 8 | program type | 1 | wiki |
| 0x10 | 8 | program version | 0 | wiki |
| 0x18 | 8 | minimum system software version | 0 | wiki |
| 0x20 | 32 | **SHA-256 of the complete ELF** | | confirmed |

According to the wiki, NPDRM-protected SELFs have a 0x20-byte content ID before the digest. These files don't.

The 0x30-byte block after it starts with a 16-bit value 3. The rest is zero. The wiki doesn't describe it, and it places the program identification header directly before the meta area. Meaning unknown.

### Meta area

`meta_size` bytes at `header_size`. In all 8 files `meta_size = num_segments * 0x50 + 0x150`.

According to the wiki, a signed SELF has a 0x50-byte certification block per segment: AES key and IV, HMAC-SHA256 hash and key. It is followed by a license header, block-map entries and an RSA signature. The wiki's tail adds up to 0x160 bytes, not 0x150, so the details may differ for these files.

Here the area is zero except for one 32-bit value 0x00010000 at offset `num_segments * 0x50 + 0x30`. That is inside what the wiki calls the license header. The tool ignores the meta area.

## The embedded ELF

The ELF header and program headers are copied verbatim. Things that differ from a Linux ELF:

- `EI_OSABI` = 9 (FreeBSD). The PS4 OS is derived from FreeBSD.
- `e_type` is in the OS-specific range. The names are from the wiki:

  | Value | Name | Here |
  |---|---|---|
  | 0xFE00 | ET_SCE_EXEC | executable without ASLR |
  | 0xFE10 | ET_SCE_DYNEXEC | executable with ASLR: the eboot |
  | 0xFE18 | ET_SCE_DYNAMIC | dynamic library: the PRXs |

- No section headers (`e_shoff` = 0, `e_shnum` = 0). readelf shows only the program headers.
- The eboot's virtual addresses start at 0, so the executable is position-independent and the loader picks the base. That fits "with ASLR".
- The eboot's entry point is 0xA0.

Program header types seen:

| `p_type` | Our name | Notes |
|---|---|---|
| 0x1 | PT_LOAD | Code (R E) and data (RW). Data has `p_memsz` > `p_filesz` (bss). |
| 0x2 | PT_DYNAMIC | `p_vaddr` = 0. The file data lies inside the DYNLIBDATA segment, which isn't loaded. |
| 0x3 | PT_INTERP | eboot only. `/libexec/ld-elf.so.1`, at the start of the code segment |
| 0x7 | PT_TLS | |
| 0x6474E550 | PT_GNU_EH_FRAME | |
| 0x61000000 | PT_SCE_DYNLIBDATA | Not loaded (`p_memsz` = 0). Holds dynamic linking data. Imports and NIDs are in here (a later M1 task). |
| 0x61000001 | PT_SCE_PROCPARAM | eboot only, at the start of the data segment |
| 0x61000002 | PT_SCE_MODULEPARAM | PRX only, same place |
| 0x6FFFFF00 | PT_SCE_COMMENT | Not loaded. eboot and libc only. |
| 0x6FFFFF01 | PT_SCE_LIBVERSION | Not loaded. Stored as the file trailer. |

The `PT_SCE_*` names are our own descriptive names. The wiki page lists only the standard types.

## Rebuild algorithm

1. Check the file's SHA-256 against the pinned build.
2. Parse the header, the entries and the ELF header + program headers.
3. Allocate `max(p_offset + p_filesz)` zero bytes. Copy the ELF header and program headers to offset 0.
4. For each data entry, copy its bytes to `phdr[ID].p_offset`.
5. Copy the trailer (bytes after `file_size`) to the `PT_SCE_LIBVERSION` offset.
6. Check that every program header with `p_filesz` > 0 is covered.
7. Compare SHA-256 of the result with the digest in the program identification header.

The gaps between segments (for example from the end of the program headers to 0x4000) are zero-filled. The digest match shows they are zero in the original ELF too.

## Open questions

- Header field 0x1A and the constant at 0x04.
- The 0x30-byte block between the program identification header and the meta area.
- The layout of the meta area's tail (0x150 bytes here vs. 0x160 on the wiki).
- Bits 12–15 and 16 of the segment flags are inferred from our files only.

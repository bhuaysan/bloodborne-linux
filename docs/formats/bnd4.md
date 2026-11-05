# BND4 (file archive)

The decompressed shader archives (see [dcx.md](dcx.md)) are BND4 archives: a header, a table of fixed-size entries, a table of UTF-16 names, and the file data. `tools/shaderbnd` reads them. What the files inside hold is in [shader-binary.md](shader-binary.md).

## Sources and confidence

Everything here was worked out from the 8 pinned shader archives with a hex dump and Python. No external format material was consulted. Field names are ours. Only the variant these 8 archives use is described; other variants may exist in other game files.

The main checks, on all 8 archives (18,517 entries): every entry's data lies after the name table and inside the file, the data regions are in table order, each is aligned to 16 and separated from the next only by padding below 16 bytes, the last one ends at the end of the file, and every name is a NUL-terminated UTF-16 string inside the name table. The tool itself checks only the bounds, sizes and names.

Confidence:
- **confirmed**: checked by parsing all 8 archives as described
- **observed**: same value in all 8 archives, meaning not verified
- **unknown**: not interpreted

## Header (0x40 bytes)

All values are little-endian.

| Off | Size | Field | Value in all 8 archives | Confidence |
|---|---|---|---|---|
| 0x00 | 4 | magic `BND4` | | confirmed |
| 0x04 | 8 | unknown | bytes `00 00 00 00 00 00 01 00` | unknown |
| 0x0C | 4 | entry count | | confirmed |
| 0x10 | 8 | header size: offset of the entry table | 0x40 | confirmed |
| 0x18 | 8 | version string, NUL-padded ASCII | `10G29R1` | observed |
| 0x20 | 8 | entry size | 0x24 | confirmed |
| 0x28 | 8 | end of the name table | | confirmed |
| 0x30 | 1 | names are UTF-16 (our reading) | 1 | confirmed (names decode as UTF-16LE) |
| 0x31 | 1 | format byte | 0x54 or 0x74 | unknown |
| 0x32 | 6 | zero | 0 | observed |
| 0x38 | 8 | zero | 0 | observed |

The format byte is 0x74 in `gxdecal`, `gxffxshader` and `gxrenderershader`, and 0x54 in the other five. The entry layout is the same for both, so what the 0x20 bit means is unknown. The tool accepts these two values only.

## Entry (0x24 bytes)

The table follows the header, in entry-count rows.

| Off | Size | Field | Value | Confidence |
|---|---|---|---|---|
| 0x00 | 1 | flags | 0x40 in every entry | unknown |
| 0x01 | 3 | zero | 0 | observed |
| 0x04 | 4 | unknown | -1 in every entry | unknown |
| 0x08 | 8 | stored size (our reading) | equals the size at 0x10 | observed |
| 0x10 | 8 | size | | confirmed |
| 0x18 | 4 | offset of the data | | confirmed |
| 0x1C | 4 | ID | | observed: unique and ascending |
| 0x20 | 4 | offset of the name | | confirmed |

The IDs count up from 0 in six archives. In the two `gxflvershader` archives they ascend with gaps (up to 14,754 for 14,571 entries). The tool doesn't use them.

Because both sizes are always equal, the entries are taken to be uncompressed. The tool refuses an entry whose sizes differ, or whose flags or the -1 field have another value.

## Names and data

The names follow the entry table directly, in table order, each a NUL-terminated UTF-16LE string. The name table ends at the offset in the header field 0x28. Names may contain backslashes as path separators (`Lightprepass0\Decal_BlendWeight.ppo`, `Filter\GXEnvDecal.ppo`), and characters such as `@[cl]`. The tool writes them as subdirectories, and refuses names with empty, `.` or `..` parts.

The data starts at the end of the name table rounded up to 16. Entries are stored in table order, each aligned to 16.

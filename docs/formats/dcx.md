# DCX (compressed file wrapper)

The game's shader archives (`dvdroot_ps4/shader/*.dcx`) are DCX files: a fixed header followed by one zlib stream. Inside is a BND4 archive (see [bnd4.md](bnd4.md)). `tools/shaderbnd` unpacks them.

## Sources and confidence

Everything here was worked out from the 8 pinned shader archives (see `tools/common/shaderarchives.py`) with a hex dump and Python's `zlib` module. No external format material was consulted. The four-letter tags are the ASCII bytes in the files. Field names are ours.

The main check: for all 8 files the stream inflates to exactly the decompressed size in the header, the compressed size reaches exactly to the end of the file, and the result parses as a BND4 archive.

Confidence:
- **confirmed**: checked by decompression or parsing on all 8 files
- **observed**: same value in all 8 files, meaning not verified
- **unknown**: not interpreted

## Layout

The header is **big-endian**, unlike the BND4 archive inside it.

| Off | Size | Field | Value in all 8 files | Confidence |
|---|---|---|---|---|
| 0x00 | 4 | magic `DCX\0` | | confirmed |
| 0x04 | 4 | version (our guess) | 0x00010000 | observed |
| 0x08 | 4 | offset of the `DCS` chunk | 0x18 | observed (matches) |
| 0x0C | 4 | offset of the `DCP` chunk | 0x24 | observed (matches) |
| 0x10 | 4 | offset of the `DCA` chunk | 0x44 | observed (matches) |
| 0x14 | 4 | offset of the zlib stream | 0x4C | observed (matches) |
| 0x18 | 4 | tag `DCS\0` | | observed |
| 0x1C | 4 | decompressed size | | confirmed |
| 0x20 | 4 | compressed size: the zlib stream, header and checksum included | | confirmed |
| 0x24 | 4 | tag `DCP\0` | | observed |
| 0x28 | 4 | method `DFLT` | | observed |
| 0x2C | 4 | size of the `DCP` chunk | 0x20 | observed (0x24 + 0x20 = 0x44) |
| 0x30 | 4 | unknown | 9 | unknown |
| 0x34 | 12 | unknown | 0 | unknown |
| 0x40 | 4 | unknown | bytes `00 01 01 00` | unknown |
| 0x44 | 4 | tag `DCA\0` | | observed |
| 0x48 | 4 | size of the `DCA` chunk | 8 | observed (0x44 + 8 = 0x4C) |
| 0x4C | | zlib stream (RFC 1950 header `78 DA`, then deflate) | | confirmed |

The value 9 at 0x30 may be the compression level: the zlib header byte `DA` marks maximum compression. That is not verified.

`DFLT` is the only method seen. The tool refuses every header that differs from the table above in a field marked observed or unknown, so a different variant fails loudly instead of being misread.

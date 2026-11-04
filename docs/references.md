# References and credits

Every external source consulted for this project is listed here. Off-limits sources (other game decomp projects, PS4 emulator source code, leaked SDKs) are never used. See `AGENTS.md`.

## Tools

| Tool | License | Used for |
|---|---|---|
| [Ghidra](https://github.com/NationalSecurityAgency/ghidra) 12.1.4 | Apache-2.0 | Disassembly and decompiler-assisted analysis |
| [REA](https://github.com/morluto/rea) 6.8.0 | MIT | Agent-driven reverse engineering workflow on top of Ghidra |
| PyGhidra 3.1.0 and JPype 1.5.2 (bundled with Ghidra) | Apache-2.0 | Running headless Ghidra scripts in Python 3 (`tools/ghidra-sync`) |
| [actions/checkout](https://github.com/actions/checkout) v7.0.1 (pinned by commit SHA) | MIT | Checking out the repository in the GitHub Actions CI (`.github/workflows/ci.yml`) |
| GNU binutils (`objdump`, `readelf`) | GPL-3.0 | Quick inspection of ELF files |
| GNU coreutils 9.10 `sha256sum` (installed) | GPL-3.0+ | Expected digests for the SHA-256 unit tests (`tests/common/sha256_test.cpp`) |
| libelf (elfutils) | LGPL-3.0+ / GPL-2.0+ | Cross-check of the SysV hash function used by `tools/elf2std` (test vectors) |
| [Linux man-pages](https://www.kernel.org/doc/man-pages/) 6.13 (installed) | Various free licenses | Page names of sections 2 and 3 as NID candidate names and as input to the derivation rules (`tools/nidmatch/syscandidates.py`, `derive.py`); `mmap(2)` (`MAP_FIXED_NOREPLACE`, `MAP_FIXED`) and `mprotect(2)` semantics for the loader (`docs/design/runtime.md`) |
| AddressSanitizer runtime of LLVM compiler-rt 22 (`libclang_rt.asan.so`, installed), its memory layout printed with `ASAN_OPTIONS=verbosity=1` and its flag defaults with `ASAN_OPTIONS=help=1` | Apache-2.0 with LLVM exception | Which address ranges ASan reserves on x86-64 Linux, for the default load base, and that `detect_stack_use_after_return` is on by default (`docs/design/runtime.md`) |
| GNU C Library 2.43 and libstdc++ 16.2 (installed `libc.so.6`, `libm.so.6`, `libstdc++.so.6`) | LGPL-2.1+ / GPL-3.0+ with runtime exception | Their defined symbol names as NID candidate names and as input to the derivation rules (`tools/nidmatch/syscandidates.py`, `derive.py`) |

## Documentation

| Source | Used for | Date |
|---|---|---|
| [SELF - SPRX, PS4 Developer wiki](https://www.psdevwiki.com/ps4/SELF_-_SPRX) (revision 296988, GFDL 1.2) | SELF header and segment flag names, program identification header, `e_type` names (`docs/formats/self.md`) | 2026-11-04 |
| [System V Application Binary Interface, generic ABI: Object Files and Program Loading](https://www.sco.com/developers/gabi/latest/contents.html) | Standard dynamic tag values, `Elf64_Sym`/`Elf64_Rela` layouts, hash table layout and hash function, section header layout, types and flags, symbol binding and section index semantics (`docs/formats/elf-dynamic.md`, `docs/formats/eh-frame.md`, `tools/elf2std`, `docs/design/runtime.md`) | 2026-11-04 |
| [Linux Standard Base Core Specification 5.0, Generic part, chapter "Exception Frames"](https://refspecs.linuxfoundation.org/LSB_5.0.0/LSB-Core-generic/LSB-Core-generic/ehframechpt.html) (`.eh_frame`, `.eh_frame_hdr`) | Record layout of `.eh_frame` (length, extended length, CIE id, zero terminator), layout of `.eh_frame_hdr` and the `DW_EH_PE` pointer encodings (`docs/formats/eh-frame.md`, `tools/elf2std`, `tools/noreturn`) | 2026-11-04 |
| [DWARF Debugging Information Format, Version 4](https://dwarfstd.org/doc/DWARF4.pdf), section 6.4 "Call Frame Information" | CIE and FDE fields and augmentation, as background for the `.eh_frame` records, and the CIE fields `tools/noreturn` skips to reach the FDE pointer encoding (`docs/formats/eh-frame.md`, `tools/noreturn`) | 2026-11-04 |
| [Intel 64 and IA-32 Architectures Software Developer's Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html), Volume 2, instructions `NOP` (recommended multi-byte forms), `CALL` (`E8` rel32), `JMP` (`FF /4`), `INT3` and `MOV` (`REX.W B8+r` imm64) | Encodings of the PLT stub jump, direct calls and padding that `tools/noreturn` matches; the import stub thunk and the hook patch (`docs/design/runtime.md`) | 2026-11-04 |
| [System V ABI, AMD64 Architecture Processor Supplement](https://gitlab.com/x86-psABIs/x86-64-ABI) | x86-64 relocation types, their calculations and PLT/GOT layout; scratch, argument and return registers and stack alignment (`docs/formats/elf-dynamic.md`, `tools/noreturn`, `docs/design/runtime.md`) | 2026-11-04 |
| [Keys, PS4 Developer wiki](https://www.psdevwiki.com/ps4/Keys) (revision 297425, GFDL 1.2), section "PS4 NID default_suffix" only | That NIDs are SHA-1 over the symbol name with a suffix appended, and the suffix value (`docs/formats/nid.md`, `tools/common/nid.py`). Byte order and encoding are our own findings | 2026-11-04 |
| Ghidra API documentation 12.1.4 (`docs/GhidraAPI_javadoc.zip`, shipped with Ghidra), classes `CParser`, `StandAloneDataTypeManager`, `DataTypeManager`, `Structure`, `Composite` and `DataTypeConflictHandler` | Parsing `symbols/types/*.h` into a scratch data type manager and copying the types into a program (`tools/ghidra-sync`) | 2026-11-04 |
| [FIPS 180-4, Secure Hash Standard](https://doi.org/10.6028/NIST.FIPS.180-4) (NIST, 2015) | SHA-256: constants, padding, message schedule and compression (`src/common/sha256.cpp`) | 2026-11-04 |

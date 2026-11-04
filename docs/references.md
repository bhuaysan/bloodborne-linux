# References and credits

Every external source consulted for this project is listed here. Off-limits sources (other game decomp projects, PS4 emulator source code, leaked SDKs) are never used. See `AGENTS.md`.

## Tools

| Tool | License | Used for |
|---|---|---|
| [Ghidra](https://github.com/NationalSecurityAgency/ghidra) 12.1.4 | Apache-2.0 | Disassembly and decompiler-assisted analysis |
| [REA](https://github.com/morluto/rea) 6.8.0 | MIT | Agent-driven reverse engineering workflow on top of Ghidra |
| GNU binutils (`objdump`, `readelf`) | GPL-3.0 | Quick inspection of ELF files |
| libelf (elfutils) | LGPL-3.0+ / GPL-2.0+ | Cross-check of the SysV hash function used by `tools/elf2std` (test vectors) |

## Documentation

| Source | Used for | Date |
|---|---|---|
| [SELF - SPRX, PS4 Developer wiki](https://www.psdevwiki.com/ps4/SELF_-_SPRX) (revision 296988, GFDL 1.2) | SELF header and segment flag names, program identification header, `e_type` names (`docs/formats/self.md`) | 2026-11-04 |
| [System V Application Binary Interface, generic ABI: Object Files and Program Loading](https://www.sco.com/developers/gabi/latest/contents.html) | Standard dynamic tag values, `Elf64_Sym`/`Elf64_Rela` layouts, hash table layout and hash function (`docs/formats/elf-dynamic.md`, `tools/elf2std`) | 2026-11-04 |
| [System V ABI, AMD64 Architecture Processor Supplement](https://gitlab.com/x86-psABIs/x86-64-ABI) | x86-64 relocation types and PLT/GOT layout (`docs/formats/elf-dynamic.md`) | 2026-11-04 |

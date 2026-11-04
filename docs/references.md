# References and credits

Every external source consulted for this project is listed here. Off-limits sources (other game decomp projects, PS4 emulator source code, leaked SDKs) are never used. See `AGENTS.md`.

## Tools

| Tool | License | Used for |
|---|---|---|
| [Ghidra](https://github.com/NationalSecurityAgency/ghidra) 12.1.4 | Apache-2.0 | Disassembly and decompiler-assisted analysis |
| [REA](https://github.com/morluto/rea) 6.8.0 | MIT | Agent-driven reverse engineering workflow on top of Ghidra |
| GNU binutils (`objdump`, `readelf`) | GPL-3.0 | Quick inspection of ELF files |

## Documentation

| Source | Used for | Date |
|---|---|---|
| [SELF - SPRX, PS4 Developer wiki](https://www.psdevwiki.com/ps4/SELF_-_SPRX) (revision 296988, GFDL 1.2) | SELF header and segment flag names, program identification header, `e_type` names (`docs/formats/self.md`) | 2026-11-04 |

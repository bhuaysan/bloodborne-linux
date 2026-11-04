# NIDs

PS4 ELFs import and export symbols by **NID**, a hash of the symbol name, instead of by the name itself. The symbol name format is in [elf-dynamic.md](elf-dynamic.md#symbols). This note covers how a NID is computed and how we recover names from it. The rules for which names we may accept are in the NID section of [clean-room.md](../clean-room.md).

## Hash

```
digest = SHA-1(name ‖ suffix)                      suffix = 51 8D 64 A6 35 DE D8 C1 E6 B0 39 B1 C3 E5 52 30
value  = digest[0..7] as a little-endian 64-bit number
NID    = base64(value as 8 big-endian bytes), '/' → '-', '=' padding dropped   (11 characters)
```

In other words, the 8 bytes `digest[7], digest[6], …, digest[0]` are base64-encoded. Eight bytes are 64 bits and 11 base64 digits hold 66, so the last digit carries only 4 bits and its low 2 bits are always 0. `tools/common/nid.py` implements this.

| Fact | Source | Confidence |
|---|---|---|
| SHA-1 over the name with a 16-byte suffix appended, and the suffix value | psdevwiki Keys page (see [references.md](../references.md)) | confirmed |
| First 8 digest bytes, reversed (little-endian) | ours: the only variant that matches, see below | confirmed |
| base64 with `+` and `-`, no padding | ours: the alphabet seen in all 8 files, and the matches below | confirmed |

**How the byte order was settled.** We hashed 15 standard C names (`strlen`, `memcpy`, `memset`, `strcmp`, `malloc`, `free`, `printf`, `fopen`, `qsort`, `abort`, `exit`, `sprintf`, `strncpy`, `atoi`, `time`) and looked them up among the 2140 exports of the pinned libc.prx. Taking the first 8 bytes in reverse order matched all 15. The first 8 bytes in order, and the last 8 in either order, matched none. With 64-bit hashes, a chance match is not plausible.

**Checked against the code.** Two of the matched exports were read in the disassembly:
- `strlen` (`0x2f970` in libc.prx) scans 8 bytes at a time for a zero byte and returns the length.
- `strcmp` (`0x30160`) compares bytewise up to alignment, then 8 bytes at a time.

These pairs are the test vectors in `tools/nidmatch/test_nidmatch.py`.

## Name matching

`tools/nidmatch/nidmatch.py` hashes candidate names and keeps those whose NID appears among the imports or exports of the 8 pinned files. Each match records the sources of its candidate name. Results go to `symbols/nids.csv`, one row per matched (NID, library). Unmatched NIDs go to `analysis/nids/unmatched.csv`, which stays local.

Candidate sources and their labels in the `source` column:

| Label | Candidates | Where they live |
|---|---|---|
| `strings:<file>` | identifier-like byte runs in one of the pinned ELFs | read from `analysis/elf/` on every run |
| `man2`, `man3` | page names of the installed Linux manual pages (every directory `manpath` lists) | `analysis/nids/candidates/`, written by `syscandidates.py` |
| `glibc` | symbols defined by the system's `libc.so.6` and `libm.so.6` | same |
| `libstdcxx` | symbols defined by the system's `libstdc++.so.6` (mangled C++ names) | same |
| *file stem* | hand-picked names, each with a comment saying why | `symbols/candidates/*.txt`, committed |

A label is a file stem, so candidate file names may use only letters, digits and `_.+-`. Labels are joined with `;` in the `source` column.

Rows already in `symbols/nids.csv` are hashed again on every run. Their labels are handled like this:
- `strings:*` labels and the labels of the committed files in `symbols/candidates/` are not carried over. Those sources are read on every run, so their labels are derived again. A name taken out of a committed file loses that label.
- Labels of the local lists and of extra files are carried over, so a run on a machine without them keeps their names.
- `--withdraw LABEL` drops a label that would be carried over, for example after deleting its candidate file.

A row left without a label is dropped unless the run finds the name again.

### First results

Matched 1508 of 3000 (NID, library) pairs:
- **Bundled PRXs:** libc.prx 1251 of its 2140 exports and libSceFios2.prx 133 of 165. The other five bundled PRXs have no matches yet.
- **Firmware imports of the static load set:** 86 of 539.
- **Best covered firmware libraries**, counted over all 8 files:
  - libScePosix 31 of 32, all from glibc and the manual pages. 10 of them also appear as strings in the eboot.
  - libSceLibcInternal 52 of 70: 41 from glibc and the manual pages, 9 mangled C++ names from libstdc++, and 2 only as strings in libc.prx.
  - libSceNet 20 of 35, all from strings in the eboot.
  - libkernel 20 of 140: 19 POSIX names from glibc and the manual pages, and `scePthreadAttrSetstacksize` from a string in the eboot.
  - libSceMsgDialog 1 (`sceMsgDialogOpen`, a string in the eboot).
- **No matches yet:** all `sce*` libraries whose function names don't appear as strings, for example GNM, pad, audio, save data and the PSN libraries.

The libc.prx matches are C names (from glibc and the manual pages) and C++ standard library names in mangled form (from libstdc++, e.g. `std::moneypunct` members). So libc.prx also contains the C++ standard library, and its mangled names agree with those of libstdc++ for more than 700 symbols.

Whether the game loads the five extra bundled PRXs at runtime is still open. The likely loader functions in libSceSysmodule and libkernel are not matched yet.

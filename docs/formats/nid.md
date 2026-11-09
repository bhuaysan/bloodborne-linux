# NIDs

PS4 ELFs import and export symbols by **NID**, a hash of the symbol name, instead of by the name itself. The symbol name format is in [elf-dynamic.md](elf-dynamic.md#symbols). This note covers how a NID is computed and how we recover names from it. The rules for which names we may accept are in the NID section of [source-policy.md](../source-policy.md).

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
| `freebsd-man2`, `freebsd-man3` | every name documented by a FreeBSD 10.4-RELEASE manual page of section 2 or 3, from the man.freebsd.org apropos index (the result pages of the query `.`, saved under `analysis/refs/`) | `analysis/nids/candidates/`, written by `freebsdman.py` |
| `shadps4-<commit>` | the function identifiers of the `LIB_FUNCTION` registrations in shadPS4's `src/core/libraries/`, see [Public name lists](#public-name-lists) | `analysis/nids/candidates/`, written by `shadps4names.py` |
| `derive-posix`, `derive-words` | `sce*` names derived from names we already have, see [Derivation rules](#derivation-rules) | `analysis/nids/candidates/`, written by `derive.py` |
| *file stem* | hand-picked names, each with a comment saying why | `symbols/candidates/*.txt`, committed |

A label is a file stem, so candidate file names may use only letters, digits and `_.+-`. Labels are joined with `;` in the `source` column.

Rows already in `symbols/nids.csv` are hashed again on every run. Their labels are handled like this:
- `strings:*` labels and the labels of every candidate file the run reads (committed, local or extra) are not carried over. Those sources are read again, so their labels are derived again. A name taken out of such a file loses that label, for example when a rule in `derive.py` is tightened and the list is written again.
- Labels of files the run doesn't have are carried over, so a run on a machine without the local lists keeps their names.
- `--withdraw LABEL` drops a label that would be carried over, for example after deleting its candidate file.

A row left without a label is dropped unless the run finds the name again.

### Derivation rules

Most `sce*` function names don't appear as strings anywhere in the dump. `tools/nidmatch/derive.py` builds them from names we already have, by rules of its own, described below. It needs the lists of `syscandidates.py` and `freebsdman.py` and takes several minutes on all cores. It writes only names whose NID matched, so `nidmatch` then records them like any other local list. Run `derive.py`, then `nidmatch.py`.

**Prefixes.** A library's function names start with a prefix taken from the library's name. Our string matches show this: `libSceNet` → `sceNetSocket`, `libSceMsgDialog` → `sceMsgDialogOpen`, `libSceFios2` → `sceFiosFHOpen`. They also show that the prefix can be shorter than the library name: the `2` is dropped in Fios, and the libkernel import `scePthreadAttrSetstacksize` has only `sce`. So a library gets `sce` followed by every leading run of the words in its name, with and without trailing digits. For example, `libSceNpMatching2` gets `sceNp`, `sceNpMatching` and `sceNpMatching2`. libkernel gets only the bare `sce`: no match shows a prefix made from the name `libkernel` itself, so the rules don't make one up. Only `libSce*` libraries and libkernel get prefixes.

| Label | Rule | Motivating matches |
|---|---|---|
| `derive-posix` | prefix + a C name from glibc or the Linux or FreeBSD manual pages, CamelCased at its underscores (leading underscores dropped) | `socket`, `bind`, `listen`, `accept`, `connect`, `getsockname`, `setsockopt`, `shutdown` → `sceNet…`; `pthread_attr_setstacksize` → `scePthreadAttrSetstacksize` |
| `derive-words` | prefix + 1 to 3 words from the matched `sce*` names, or prefix + 1 or 2 words from a larger vocabulary (below) | Our `sce*` matches are made of a small set of words: `sceFiosFHOpen`, `sceFiosFHOpenSync`, `sceFiosDHOpen`, `sceNetEpollCreate`, `sceNetPoolCreate`, `sceNetResolverCreate`, … |

The larger vocabulary adds two sources to the words of the matched `sce*` names:
- the parts of the C names;
- the CamelCase words that occur in at least 2 identifiers in the pinned ELFs.

The game's identifiers use the same words as the API. Leaving out the `sce*` names themselves, 69 of the 110 words in our string-matched `sce*` names occur in at least 2 of the dump's identifiers.

`derive-words` repeats until nothing new turns up. Every new match adds its words to the vocabulary, and the next round tries only combinations that contain a new word. A name is kept only if its NID belongs to a library that its prefix came from. Names from earlier runs that came only from these rules are not used as input, so a rerun gives the same lists.

**No excluded libraries.** Under the earlier policy the libraries of the exposure log (libSceRtc, libSceSaveData, libSceSaveDataDialog, libSceRegMgr, libSceRemoteplay) got no prefixes and no derived names. [source-policy.md](../source-policy.md) lifted that on 2026-11-06 and #226 removed it from the tools: every library gets prefixes, every NID is a target, and a name from a derivation rule alone is accepted for any library.

### Public name lists

The source policy allows public NID lists and emulator code. Whatever the source, the rule stays: a name counts only if it hashes to a NID of the pinned files. So a list is just one more candidate file, and its file stem is its label, which names the list and its revision, e.g. `shadps4-0d5f273` for shadPS4 at commit `0d5f273`. The label ends up in the `source` column, and the source gets a row in [references.md](../references.md).

`tools/nidmatch/shadps4names.py` writes such a list from a shadPS4 checkout. It takes the function identifiers of the `LIB_FUNCTION("<nid>", "<library>", <version>, "<module>", <function>)` registrations (and the C name behind each `posix_*` identifier) and ignores the NIDs listed beside them, so a wrong pairing in the emulator can't name a NID here. The list is local, like the other generated ones, and the label is carried over in `nids.csv` on machines without it. A list that is not code, such as a table saved from a wiki, goes in the same way as a candidate file under `symbols/candidates/` or as an extra file argument, with a stem that names the page and its revision.

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

### With derivation rules

Matched 1784 of 3000 (NID, library) pairs. **Firmware imports of the static load set: 328 of 539**, up from 86.
- **276 new rows:**
  - 204 come only from `derive-words`.
  - 72 come from both rules.
  - No new row comes only from `derive-posix`. A C name in CamelCase is also a word combination, so `derive-words` finds those names as well.
  - The `sceKernel*` names come only from `derive-words`, as the bare `sce` + 2 words. `Kernel` is a word of the C name `get_kernel_syms`, so `sceKernelOpen` is `sce` + `Kernel` + `Open`. Names with 3 words after `sceKernel`, such as `sceKernelClockGettime`, are out of reach.
- **Cross-check:** the rules also find 126 names that we already had from strings, for example most `sceNet*` names and many `sceFios*` names.
- **Coverage by library:**
  - libkernel rose from 20 to 91 of 140, libSceNet from 20 to 35 of 35, and libSceAvPlayer to 14 of 16.
  - Net and several smaller libraries are complete: AudioOut, AudioIn, Mouse, MsgDialog, ImeDialog, NpSignaling, NpCommerce, NpProfileDialog, Ssl, Sysmodule and CommonDialog.
- **Bundled PRXs:** the first names in four of the five extra PRXs, for example `sceFaceDetection`, `sceHandDetection` and `sceHeadTrackerInitialize`.
- **Still weak:** libSceGnmDriver had 2 of 50 at this point. Its names need more or rarer words than the vocabulary has.

`sceSysmoduleLoadModule` is now named. With it, the question of the five extra bundled PRXs is answered in [imports.md](imports.md#modules-loaded-at-runtime).

### With a public name list

Matched 1998 of 3000 (NID, library) pairs, up from 1794. **Firmware imports of the static load set: 528 of 539.** The list is shadPS4's `LIB_FUNCTION` registrations at commit `0d5f273` (`shadps4-0d5f273`, 5155 candidate names).
- **204 new rows**, all with the label `shadps4-0d5f273`. Most are libSceGnmDriver (48, now 50 of 50), libkernel (38, now 132 of 140), the Np libraries (libSceNpMatching2 15, libSceNpManager 11, libSceNpScore 10, libSceNpWebApi 8, libSceNpUtility 7), libSceHttp 10, and three of the former exposure-log libraries (libSceRtc 10, libSceSaveData 11, libSceSaveDataDialog 4).
- **Former exposure-log libraries:** libSceRtc 10 of 10, libSceSaveData 11 of 11 and libSceSaveDataDialog 4 of 4. The eboot imports nothing from libSceRegMgr or libSceRemoteplay, so there is nothing to name there.
- **Derivation rules on the five libraries:** `derive.py` ran without the exclusion, and with `--ignore-source shadps4-0d5f273` so that the list's thousands of words don't blow up the search (a first run without it was still in round 1 after 24 minutes on 4 cores). It found no name that the list lacks. It did find 13 of the 25 names of the former exposure-log libraries on its own: libSceRtc 2 of 10, libSceSaveData 7 of 11 and libSceSaveDataDialog 4 of 4. Those rows carry both labels. The public list also reached 266 names that were already matched, so those rows gained the label `shadps4-0d5f273` (the derivation rules added no label there).
- **Still unmatched:** 882 of libc.prx's 2140 NIDs, 44 of libSceSmart's 48, 21 of libSceFios2's 165, 19 of libSceFace's 29, 18 of libSceLibcInternal's 70 and 8 of libkernel's 140.

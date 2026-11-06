# Save data

What the eboot's libSceSaveData and libSceSaveDataDialog imports do, worked out only from the eboot's own call sites. Both libraries are in (or derived from) the exposure log of [clean-room.md](../clean-room.md#exposure-log), so this note and the code (`src/hle/system/save_data.{hpp,cpp}`) give NIDs and neutral descriptions, never function names. Addresses are eboot link addresses. Facts the code shows carry their addresses; values it doesn't show are **our choice**, marked as such.

## How the game uses them

The calls sit in a small task framework at 0x283c4b0–0x2844000, created by a manager at 0x1831270–0x1833400 and 0x2839540–0x283c4b0. A request (save, load, erase, store set-up) runs a directory task that mounts a save directory and reads or writes files in it, and a task that reads or writes a per-user byte store. Every task reads two per-user properties first, "User.SigninStatus" (must be non-zero) and "User.PS4.SceUserServiceUserId" (the user id it passes, string referenced at 0x283e45c); a failure gives the game's result 8 (0x283faf7).

**Directory names.** One per slot: the wide format `%s%s%04X` (0x488be24, built at 0x283e820) with an empty first string and "SPRJ" (0x49bd6f8, set at 0x1831328), so `SPRJ0000` to `SPRJ0009`; the slot check `slot <= 9` is at 0x156d835. The name is copied into a 32-byte buffer.

**Files.** Below the mount point the game names its files with the wide formats `%hS/userdata%04d` and `%hS/backup%04d` (0x488bf2e, 0x488bf50), the mount point being the `%hS` argument (0x283ff5b, 0x2842bb3). Backups are written only when a descriptor flag is set (0x2842c1b); on load a failed `userdata` read falls back to `backup` (0x283ff98).

**Results.** The game translates raw results at 0x283ee10: 0 → 0; 0x809f0008 and 0x809f000e → 6 ("no save yet" on load); 0x809f0009 and 0x809f000a → 3; 0x809f000f → 7; 0x809f0011 → 5; 0x809f0000–0x809f0007, 0x809f000b–0x809f000d, 0x809f0010 and 0x809f0012–0x809f0014 → 8; anything else → 0xa. 0x809f0010 (mount) and 0x809f0014 (search, erase, store set-up) make it sleep 16666 µs and retry.

## Records

**Mount input** (built inline by the load task, 0x283f97f–0x283fe12, and the save task, 0x2842257–0x2842456):

| Offset | Size | Field | Evidence |
|---|---|---|---|
| 0 | 4 | user id | from the property, stored at 0x283f97f |
| 8 | 8 | pointer to a 10-byte string, the first `%s` of the directory name format (getter 0x28328c0, used at 0x283e8a5), null in practice | its only setter passes null, 0x1831362 |
| 0x10 | 8 | pointer to the 32-byte directory name | 0x283fc20, 0x2842257 |
| 0x18 | 8 | pointer to a 65-byte string from the save descriptor | getter 0x2832dc0 |
| 0x20 | 8 | block count: entry sizes in 32 KiB blocks, doubled with backups, plus 0x140 | 0x283ef80 |
| 0x28 | 4 | mode: 1 on load (0x283fe12), 2 or 0xa on save, with 4 added on the retry | 0x2842481 |

**Mount output**: 0x40 bytes, zeroed by the game; the mount point is a NUL-terminated string at offset 0 (used as `%hS` at 0x283ff5b). The game passes the same record to the unmount call on every path after a successful mount (0x284001c, 0x2840096, 0x2840397, 0x2842645, …).

**Search condition** (0x283e3b0): user id at 0, a null pointer at 8, a pointer to the 32-byte directory name at 0x10, zeros at 0x18 and 0x1c. **Search result**: the hit count at 0 (written by the call), a pointer to an array of 32-byte entries at 8, the capacity (1) at 0x10. A count of 0 gives the game's result 6.

**Erase** (0x283e765, 0x283e78d): user id at 0, null at 8, a pointer to the same 32-byte name at 0x10. The game calls it after a search hit in the erase task, whose byte-store twin (0x2840510) zeroes its data, so it removes the directory.

**Byte store** calls take `(user id, buffer, byte count, offset)` (reads at 0x2840772, 0x2840dce, …, writes at 0x28407f7, 0x284182e, …); a non-zero result gives the game's result 8. The store starts with a 16-byte header `{adler32 of the entry sizes, slot, 0x7f7f7f7f, 0}` and 8 bytes per entry (0x2840dec, 0x2840ec1, adler32 at 0x283f0f0); a magic or slot mismatch gives result 6 (0x2841056), an adler32 mismatch result 7 (0x2840e11, 0x2841079, 0x2841093), a per-entry header mismatch 6 (0x2840ebb, 0x2840ec8, 0x284125b); with the optional 16-byte entry trailer, a trailer mismatch gives 7 (0x2840fab, 0x284127e). The set-up call `v7AAAMo0Lz4` takes `(user id, total size, 0x524-byte record)` with three strings at 0, 0x80 and 0x100 (0x28442b2); the game refuses a total over 0x100000 itself (0x283f040).

## The imports

| NID | Call sites | What our runtime does |
|---|---|---|
| `ZkZhskCPXFw` | 0x283c50e, `(0x24-byte record)`; non-zero fails the game's init | succeeds |
| `yKDy8S5yLA0` | 0x283c5ee, no arguments, result unchecked | succeeds |
| `32HQAQdwM2o` | 0x283fe40 (load), 0x2842472, 0x2842499 (save) | mounts `<root>/<dir>` at the lowest free `/savedataN` (**our choice** of names: the game only uses the string as a prefix). A missing directory gives 0x809f0008 unless mode bit 4 is set, which creates it: that is what makes the save path's retry (0x2842481) and the load path's "no save yet" (0x283ee10) work. Bad names, a second mount of one directory and other failures give 0x809f0001 (**our choice** within the range the game treats as one error, 8) |
| `BMR4F-Uek3E` | 14 sites, `(mount output)` | removes the mount |
| `dyIhnXq-0SM` | 0x283e3b0 | one hit with the name if the directory exists, else none |
| `S1GkePI17zQ` | 0x283e765, 0x283e78d | removes the directory; 0x809f0008 if it is missing, 0x809f0001 while mounted |
| `85zul--eGXs` | 0x2842632, 0x28427c5, 0x28429a6, `(mount output, kind 1–3, text, 0x80 or 0x400)` | logs the text and keeps nothing (**our choice**) |
| `c88Yy54Mx0w` | 0x2842ac1, `(mount output, 0x28-byte record)` | succeeds for a mounted record, keeps nothing (**our choice**) |
| `7Bt5pBC-Aco` | store reads, see above | reads `<root>/store-<user>.bin`; bytes it doesn't hold read as zeros, so a fresh store has no header and the game reports "no save yet" |
| `h3YURzXGSVQ` | store writes | writes into that file |
| `v7AAAMo0Lz4` | 0x28442b2 | logs the size and succeeds |
| `s9e3+YpRnzw` | 0x28425e7, no arguments; 0x80b80008 makes the game retry | succeeds |
| `4tPhsP6FpDI` | 0x28432f5, `(record)`: a 0x98-byte record at rsp+0x9a0 that stores its own size at offset 0x30 (0x2843155–0x28431a1, 0x28431e2); non-zero skips the poll | fails (**our choice**: no dialog UI) |
| `KK3Bdg1RWK0` | 0x284330a, polled every 16666 µs while it returns 2 | returns 0 |
| `YuH2FA7azqQ` | 0x2843314, result unchecked | succeeds |

The dialog is only reached when a save mount fails with 0x809f000a and a task flag is set (0x2841ca0); its result record is never read back and the mount error stays the task's result (0x28432b5, 0x2843323). The runtime never returns 0x809f000a, so the dialog calls are not reached in practice. The game loads and unloads system module 0xa0 around it (0x28425ce, 0x284331e).

**Our choices for the host side.** The save root is `profile_data()/savedata` (#82), created with mode 0700; a directory named `SPRJ0000` is `<root>/SPRJ0000/`, and the byte store of user 1 is `<root>/store-1.bin`. Reads and writes of the store are bounded at 4 MiB. Mount modes other than bit 4 are logged and otherwise not applied (a load mount is writable too). The block count is logged, not enforced. Save directories are shared by all users, while the byte store is per user; each task passes one user id.

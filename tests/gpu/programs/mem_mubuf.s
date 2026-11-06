; Untyped buffer memory. User data: s[0:1] the output address (0x2000-aligned), s[4:7] the V# of
; a table of dwords, s[8:11] the V# of a buffer that is stored to, s12 a soffset in bytes. Lane i
; loads with offen, idxen, both, as a dword pair and with addr64 (v10-v12, v14-v15, v19), and
; once more with only some lanes active (v13). With idxen and offen it also loads at the offsets
; 12 and 16 and as a pair from 12 (v22-v25), which are on both sides of a stride of 16. The loads
; with offen alone (v10), with idxen and offen at offset 12 (v22) and with addr64 (v19) use s12 as
; soffset, the others 0. The loads start from 0x77, to tell a dropped access from a zero. Then it
; stores with offen and as a pair.
; It writes the loaded values to out[64 * k + i].
v_lshlrev_b32_e32 v1, 2, v0             ; v1 = 4i
v_or_b32_e32 v2, s0, v1                 ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s1
v_mov_b32_e32 v5, s1
v_mov_b32_e32 v10, 0x77
v_mov_b32_e32 v11, 0x77
v_mov_b32_e32 v12, 0x77
v_mov_b32_e32 v13, 0x77
v_mov_b32_e32 v14, 0x77
v_mov_b32_e32 v15, 0x77
v_mov_b32_e32 v19, 0x77
v_mov_b32_e32 v22, 0x77
v_mov_b32_e32 v23, 0x77
v_mov_b32_e32 v24, 0x77
v_mov_b32_e32 v25, 0x77
buffer_load_dword v10, v1, s[4:7], s12 offen offset:8
buffer_load_dword v11, v0, s[4:7], 0 idxen
v_mov_b32_e32 v6, v0                    ; v[6:7] = index i, offset 4 (0x44 with the immediate)
v_mov_b32_e32 v7, 4
buffer_load_dword v12, v[6:7], s[4:7], 0 idxen offen offset:0x40
buffer_load_dword v22, v[6:7], s[4:7], s12 idxen offen offset:8  ; offset 4 + 8
buffer_load_dword v23, v[6:7], s[4:7], 0 idxen offen offset:12   ; offset 4 + 12
buffer_load_dwordx2 v[24:25], v[6:7], s[4:7], 0 idxen offen offset:8
buffer_load_dwordx2 v[14:15], v1, s[4:7], 0 offen
v_mov_b32_e32 v20, v1                   ; v[20:21] = 4i
v_mov_b32_e32 v21, 0
buffer_load_dword v19, v[20:21], s[4:7], s12 addr64
s_mov_b32 exec_lo, 0x0f0f0f0f
s_mov_b32 exec_hi, 0
buffer_load_dword v13, v1, s[4:7], 0 offen
s_mov_b64 exec, -1
v_or_b32_e32 v16, 0xa000, v0
v_or_b32_e32 v17, 0xb000, v0
v_lshlrev_b32_e32 v18, 3, v0
buffer_store_dword v16, v1, s[8:11], 0 offen
buffer_store_dwordx2 v[16:17], v18, s[8:11], 0 offen offset:0x200
s_waitcnt vmcnt(0)
flat_store_dword v[2:3], v10
v_or_b32_e32 v4, 0x100, v2
flat_store_dword v[4:5], v11
v_or_b32_e32 v4, 0x200, v2
flat_store_dword v[4:5], v12
v_or_b32_e32 v4, 0x300, v2
flat_store_dword v[4:5], v13
v_or_b32_e32 v4, 0x400, v2
flat_store_dword v[4:5], v14
v_or_b32_e32 v4, 0x500, v2
flat_store_dword v[4:5], v15
v_or_b32_e32 v4, 0x600, v2
flat_store_dword v[4:5], v19
v_or_b32_e32 v4, 0x700, v2
flat_store_dword v[4:5], v22
v_or_b32_e32 v4, 0x800, v2
flat_store_dword v[4:5], v23
v_or_b32_e32 v4, 0x900, v2
flat_store_dword v[4:5], v24
v_or_b32_e32 v4, 0xa00, v2
flat_store_dword v[4:5], v25
s_endpgm

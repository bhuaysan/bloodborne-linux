; V# modes and narrow MUBUF accesses. User data: s[0:1] the output address (0x2000-aligned), s[4:7]
; the V# of a table, s[8:11] the V# of a buffer that is stored to (both with the modes under test:
; swizzle_enable, add_tid_enable, heap), s12 a soffset in bytes. Lane i loads a dword with idxen
; (index i, offset 4) and with offen (offset 4i), a pair with idxen and offen (index i, offsets 4
; and 8), the four narrow loads at offset 3i with soffset s12 and a 32_32 UINT element with idxen
; and offen (index i, offset 4). The loads start from 0x77, to tell a dropped access from a zero.
; Then it stores 0xa000 | i as a dword with idxen (index i, soffset 0x800), as a byte at offset i
; and as a short at offset 2i (soffset 0x400).
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
v_mov_b32_e32 v16, 0x77
v_mov_b32_e32 v17, 0x77
v_mov_b32_e32 v18, 0x77
v_mov_b32_e32 v19, 0x77
v_mov_b32_e32 v6, v0                    ; v[6:7] = index i, offset 4
v_mov_b32_e32 v7, 4
v_mul_u32_u24_e32 v20, 3, v0            ; v20 = 3i
buffer_load_dword v10, v0, s[4:7], 0 idxen offset:4
buffer_load_dword v11, v1, s[4:7], 0 offen
buffer_load_dwordx2 v[12:13], v[6:7], s[4:7], 0 idxen offen
buffer_load_ubyte v14, v20, s[4:7], s12 offen
buffer_load_sbyte v15, v20, s[4:7], s12 offen
buffer_load_ushort v16, v20, s[4:7], s12 offen
buffer_load_sshort v17, v20, s[4:7], s12 offen
tbuffer_load_format_xy v[18:19], v[6:7], s[4:7], 0 format:[BUF_DATA_FORMAT_32_32,BUF_NUM_FORMAT_UINT] idxen offen
s_waitcnt vmcnt(0)
v_or_b32_e32 v21, 0xa000, v0
v_lshlrev_b32_e32 v22, 1, v0            ; v22 = 2i
s_movk_i32 s13, 0x800
s_movk_i32 s14, 0x400
buffer_store_dword v21, v0, s[8:11], s13 idxen
buffer_store_byte v21, v0, s[8:11], 0 offen
buffer_store_short v21, v22, s[8:11], s14 offen
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
flat_store_dword v[4:5], v16
v_or_b32_e32 v4, 0x700, v2
flat_store_dword v[4:5], v17
v_or_b32_e32 v4, 0x800, v2
flat_store_dword v[4:5], v18
v_or_b32_e32 v4, 0x900, v2
flat_store_dword v[4:5], v19
s_endpgm

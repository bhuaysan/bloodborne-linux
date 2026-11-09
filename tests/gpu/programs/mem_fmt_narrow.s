; The formatted MUBUF forms with fewer than four components. User data: s[4:7] the V# of a 4 KB
; buffer with the format and dst_sel under test, s[8:11] the V# of the output, s12 the log2 of
; the store's lane stride. Lane i loads the element at 16 * i with buffer_load_format_x, its
; three higher VGPRs preset to -1, and writes the four to out[16 * i]; loads it with
; buffer_load_format_xyz, its fourth VGPR preset to -1, and writes the four to
; out[0x400 + 16 * i]; and reads the four input dwords at 0x800 + 16 * i and stores only the
; first with buffer_store_format_x at 0xc00 + (i << s12), so that the three VGPRs after it hold
; nonzero values that the store must not use.
v_lshlrev_b32_e32 v1, 4, v0
v_mov_b32_e32 v17, -1
v_mov_b32_e32 v18, -1
v_mov_b32_e32 v19, -1
buffer_load_format_x v16, v1, s[4:7], 0 offen
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[16:19], v1, s[8:11], 0 offen
v_mov_b32_e32 v23, -1
buffer_load_format_xyz v[20:22], v1, s[4:7], 0 offen
s_movk_i32 s20, 0x400
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[20:23], v1, s[8:11], s20 offen
s_movk_i32 s20, 0x800
buffer_load_dwordx4 v[24:27], v1, s[4:7], s20 offen
v_lshlrev_b32_e32 v3, s12, v0
s_movk_i32 s20, 0xc00
s_waitcnt vmcnt(0)
buffer_store_format_x v24, v3, s[4:7], s20 offen
s_endpgm

; MUBUF atomics. User data: s[2:3] the EXEC of the atomics, s[4:7] the V# of a table with a row of
; 36 dwords per lane, s[8:11] the V# of the memory the atomics update, s12 the soffset of the _x2
; ones. Lane i loads its row into v[10:45], runs each atomic on a dword (0x100 + 0x100k + 4i for op
; k) or qword (s12 + 0x200k + 8i) of its own and the contended ones on the dword at 0 and the qword
; at 8, all with glc but xor, and stores the row back. The row: swap, cmpswap (src, cmp), add, sub,
; smin, umin, smax, umax, and, or, xor, inc, dec, fcmpswap (src, cmp), fmin, fmax, the contended
; inc, unused; in qwords cmpswap_x2 (src, cmp), smax_x2, dec_x2, fmin_x2, fcmpswap_x2 (src, cmp)
; and the contended add_x2.
v_mul_u32_u24_e32 v3, 0x90, v0          ; v3 = 144i, the row
v_lshlrev_b32_e32 v1, 2, v0
v_add_i32_e32 v1, vcc, 0x100, v1        ; v1 = 0x100 + 4i
v_lshlrev_b32_e32 v2, 3, v0             ; v2 = 8i
s_mov_b32 exec_lo, s2
s_mov_b32 exec_hi, s3
buffer_load_dwordx4 v[10:13], v3, s[4:7], 0 offen
buffer_load_dwordx4 v[14:17], v3, s[4:7], 0 offen offset:16
buffer_load_dwordx4 v[18:21], v3, s[4:7], 0 offen offset:32
buffer_load_dwordx4 v[22:25], v3, s[4:7], 0 offen offset:48
buffer_load_dwordx4 v[26:29], v3, s[4:7], 0 offen offset:64
buffer_load_dwordx4 v[30:33], v3, s[4:7], 0 offen offset:80
buffer_load_dwordx4 v[34:37], v3, s[4:7], 0 offen offset:96
buffer_load_dwordx4 v[38:41], v3, s[4:7], 0 offen offset:112
buffer_load_dwordx4 v[42:45], v3, s[4:7], 0 offen offset:128
s_waitcnt vmcnt(0)
buffer_atomic_swap v10, v1, s[8:11], 0 offen glc
buffer_atomic_cmpswap v[11:12], v1, s[8:11], 0 offen offset:256 glc
buffer_atomic_add v13, v1, s[8:11], 0 offen offset:512 glc
buffer_atomic_sub v14, v1, s[8:11], 0 offen offset:768 glc
buffer_atomic_smin v15, v1, s[8:11], 0 offen offset:1024 glc
buffer_atomic_umin v16, v1, s[8:11], 0 offen offset:1280 glc
buffer_atomic_smax v17, v1, s[8:11], 0 offen offset:1536 glc
buffer_atomic_umax v18, v1, s[8:11], 0 offen offset:1792 glc
buffer_atomic_and v19, v1, s[8:11], 0 offen offset:2048 glc
buffer_atomic_or v20, v1, s[8:11], 0 offen offset:2304 glc
buffer_atomic_xor v21, v1, s[8:11], 0 offen offset:2560
buffer_atomic_inc v22, v1, s[8:11], 0 offen offset:2816 glc
buffer_atomic_dec v23, v1, s[8:11], 0 offen offset:3072 glc
buffer_atomic_fcmpswap v[24:25], v1, s[8:11], 0 offen offset:3328 glc
buffer_atomic_fmin v26, v1, s[8:11], 0 offen offset:3584 glc
buffer_atomic_fmax v27, v1, s[8:11], 0 offen offset:3840 glc
buffer_atomic_inc v28, off, s[8:11], 0 glc
buffer_atomic_cmpswap_x2 v[30:33], v2, s[8:11], s12 offen glc
buffer_atomic_smax_x2 v[34:35], v2, s[8:11], s12 offen offset:512 glc
buffer_atomic_dec_x2 v[36:37], v2, s[8:11], s12 offen offset:1024 glc
buffer_atomic_fmin_x2 v[38:39], v2, s[8:11], s12 offen offset:1536 glc
buffer_atomic_fcmpswap_x2 v[40:43], v2, s[8:11], s12 offen offset:2048 glc
buffer_atomic_add_x2 v[44:45], off, s[8:11], 0 offset:8 glc
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[10:13], v3, s[4:7], 0 offen
buffer_store_dwordx4 v[14:17], v3, s[4:7], 0 offen offset:16
buffer_store_dwordx4 v[18:21], v3, s[4:7], 0 offen offset:32
buffer_store_dwordx4 v[22:25], v3, s[4:7], 0 offen offset:48
buffer_store_dwordx4 v[26:29], v3, s[4:7], 0 offen offset:64
buffer_store_dwordx4 v[30:33], v3, s[4:7], 0 offen offset:80
buffer_store_dwordx4 v[34:37], v3, s[4:7], 0 offen offset:96
buffer_store_dwordx4 v[38:41], v3, s[4:7], 0 offen offset:112
buffer_store_dwordx4 v[42:45], v3, s[4:7], 0 offen offset:128
s_endpgm

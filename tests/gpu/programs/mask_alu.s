; Mask results of the VALU. s[0:1]: the output address (512-byte aligned), s2: 12, s3: -32, s4: 10.
; Stage k stores out[64k + i], and v[2:3] moves on by 0x100 bytes after each store.
v_lshlrev_b32_e32 v1, 2, v0
v_or_b32_e32 v2, s0, v1             ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s1
v_mov_b32_e32 v10, 10
v_mov_b32_e32 v11, 20
v_mov_b32_e32 v15, 0
v_cmp_lt_u32_e32 vcc, s2, v0        ; stage 0: vcc = s2 < i
v_cndmask_b32_e32 v12, v10, v11, vcc
flat_store_dword v[2:3], v12
v_add_i32_e32 v2, vcc, 0x100, v2
v_subrev_i32_e32 v14, vcc, 32, v0   ; v14 = i - 32
v_cmp_lt_i32_e64 s[6:7], v14, 0     ; stage 1: i < 32, signed
v_cmp_lt_u32_e64 s[8:9], v14, 32    ; stage 2: 32 <= i, unsigned
v_cndmask_b32_e64 v12, v10, v11, s[6:7]
flat_store_dword v[2:3], v12
v_add_i32_e32 v2, vcc, 0x100, v2
v_cndmask_b32_e64 v12, v10, v11, s[8:9]
flat_store_dword v[2:3], v12
v_add_i32_e32 v2, vcc, 0x100, v2
v_add_i32_e32 v13, vcc, s3, v0      ; stage 3: the carry out of i + s3
v_addc_u32_e32 v16, vcc, 0, v15, vcc
flat_store_dword v[2:3], v16
v_add_i32_e32 v2, vcc, 0x100, v2
v_mov_b32_e32 v17, s4               ; stage 4: the borrow of i - s4, through VOP3b and a pair
v_sub_i32_e64 v18, s[12:13], v0, v17
v_subb_u32_e64 v19, s[14:15], 0, v15, s[12:13]
flat_store_dword v[2:3], v19
v_add_i32_e32 v2, vcc, 0x100, v2
s_mov_b32 exec_lo, 0x0f0f0f0f       ; stage 5: a compare in some lanes clears the others' bits
s_mov_b32 exec_hi, 0x00ff00ff
v_cmp_ge_u32_e32 vcc, v0, v15
s_mov_b64 exec, -1
v_cndmask_b32_e32 v12, v10, v11, vcc
flat_store_dword v[2:3], v12
v_add_i32_e32 v2, vcc, 0x100, v2
v_cmpx_gt_u32_e64 s[16:17], 40, v0  ; stage 6: v_cmpx into a pair sets EXEC and the pair
v_mov_b32_e32 v21, 1                ; (lanes 0-39), which only they write
s_mov_b64 exec, -1
v_cndmask_b32_e64 v12, v15, v10, s[16:17]
v_add_i32_e32 v12, vcc, v12, v21
flat_store_dword v[2:3], v12
v_add_i32_e32 v2, vcc, 0x100, v2
v_and_b32_e32 v4, 1, v0             ; stage 7: carry in c = bit 0 of the lane, a = -1 if bit 1
v_cmp_eq_u32_e64 s[6:7], 1, v4
v_and_b32_e32 v4, 2, v0
v_cmp_eq_u32_e64 s[8:9], 2, v4
v_cndmask_b32_e64 v5, 0, -1, s[8:9]
v_mov_b32_e32 v7, 0                 ; four carry bits, the first one highest:
v_addc_u32_e64 v6, vcc, v5, v15, s[6:7]     ; the carry of a + c
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_subb_u32_e64 v6, vcc, v5, v15, s[6:7]     ; the borrow of a - c
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_subbrev_u32_e64 v6, vcc, v5, v15, s[6:7]  ; the borrow of 0 - a - c
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_subrev_i32_e32 v6, vcc, v5, v0    ; the borrow of i - a
v_addc_u32_e32 v7, vcc, v7, v7, vcc
flat_store_dword v[2:3], v7
s_endpgm

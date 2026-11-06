; The LDS atomics, in 128 lanes. s[0:1]: the output address (512-byte aligned). Dwords 0 to 15 of
; the LDS are the operands, which every lane updates; out[i] is dword i at the end. out[128 + i],
; out[256 + i] and out[384 + i] hold what the exchange, the compare-swap and the add return.
s_mov_b32 m0, -1
v_lshlrev_b32_e32 v1, 2, v0
v_or_b32_e32 v2, s0, v1             ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s1
v_mov_b32_e32 v30, 0
ds_write_b32 v1, v30                ; dwords 0-127 = 0
s_waitcnt lgkmcnt(0)
s_barrier
v_mov_b32_e32 v10, -1               ; the start values that are not 0
ds_write_b32 v30, v10 offset:16     ; and
ds_write_b32 v30, v10 offset:20     ; min_u
v_mov_b32_e32 v10, 0x7fffffff
ds_write_b32 v30, v10 offset:28     ; min_i
v_mov_b32_e32 v10, 8
ds_write_b32 v30, v10 offset:40     ; inc
v_mov_b32_e32 v10, 2
ds_write_b32 v30, v10 offset:44     ; dec
v_mov_b32_e32 v10, 100
ds_write_b32 v30, v10 offset:48     ; rsub
v_mov_b32_e32 v10, 0xf0f0f0f0
ds_write_b32 v30, v10 offset:52     ; mskor
v_mov_b32_e32 v10, 5
ds_write_b32 v30, v10 offset:56     ; cmpst
s_waitcnt lgkmcnt(0)
s_barrier
v_add_i32_e32 v4, vcc, 1, v0        ; i + 1
v_mov_b32_e32 v8, 1
v_lshlrev_b32_e32 v5, v0, v8        ; 1 << (i & 31)
v_xor_b32_e32 v6, -1, v5
v_add_i32_e32 v7, vcc, 5, v0        ; i + 5
v_subrev_i32_e32 v9, vcc, 64, v0    ; i - 64
ds_add_u32 v30, v4
ds_sub_u32 v30, v4 offset:4
ds_or_b32 v30, v5 offset:8
ds_xor_b32 v30, v4 offset:12
ds_and_b32 v30, v6 offset:16
ds_min_u32 v30, v7 offset:20
ds_max_u32 v30, v0 offset:24
ds_min_i32 v30, v9 offset:28
ds_max_i32 v30, v9 offset:32
ds_wrxchg_rtn_b32 v12, v30, v4 offset:36
v_mov_b32_e32 v11, 10
ds_inc_u32 v30, v11 offset:40
v_mov_b32_e32 v11, 5
ds_dec_u32 v30, v11 offset:44
v_mov_b32_e32 v11, 30
ds_rsub_u32 v30, v11 offset:48
v_mov_b32_e32 v11, 0x00ff00ff
v_mov_b32_e32 v13, 0x12003400
ds_mskor_b32 v30, v11, v13 offset:52
v_mov_b32_e32 v11, 5
v_mov_b32_e32 v13, 9
ds_cmpst_rtn_b32 v14, v30, v11, v13 offset:56
ds_add_rtn_u32 v15, v30, v8 offset:60
s_waitcnt lgkmcnt(0)
s_barrier
ds_read_b32 v16, v1
s_waitcnt lgkmcnt(0)
flat_store_dword v[2:3], v16
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v12
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v14
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v15
s_endpgm

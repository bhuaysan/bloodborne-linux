; LDS traffic between the lanes of one wave, with no s_barrier, in 64 lanes. s[0:1]: the output
; address (256-byte aligned). The hardware runs the DS instructions of a wave in order, so lane i
; sees the write of lane i ^ 1 that came before its read, and not the one that came after it.
; out[i] = (i ^ 1) + 7 and out[64 + i] = i + 107.
s_mov_b32 m0, -1
v_lshlrev_b32_e32 v1, 2, v0         ; v1 = 4i
v_or_b32_e32 v2, s0, v1             ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s1
v_add_i32_e32 v4, vcc, 7, v0
ds_write_b32 v1, v4                 ; LDS[i] = i + 7
v_xor_b32_e32 v5, 1, v0
v_lshlrev_b32_e32 v5, 2, v5         ; v5 = 4 (i ^ 1)
ds_read_b32 v6, v5                  ; the neighbour's i + 7
s_waitcnt lgkmcnt(0)
flat_store_dword v[2:3], v6
v_add_i32_e32 v2, vcc, 0x100, v2
v_add_i32_e32 v7, vcc, 100, v6
ds_write_b32 v1, v7                 ; LDS[i] = (i ^ 1) + 107, after the neighbour's read
ds_read_b32 v8, v5                  ; the neighbour's new value: i + 107
s_waitcnt lgkmcnt(0)
flat_store_dword v[2:3], v8
s_endpgm

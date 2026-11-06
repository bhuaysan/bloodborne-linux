; The GPU memory mirror (#268). s[0:1]: the output address (256-byte aligned), s[2:3]: the input
; address. Lane i writes input[0] + i to out[i].
s_load_dword s16, s[2:3], 0x0
s_waitcnt lgkmcnt(0)
v_lshlrev_b32_e32 v1, 2, v0         ; v1 = 4 * i
v_or_b32_e32 v2, s0, v1             ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s1
v_add_i32_e32 v4, vcc, s16, v0
flat_store_dword v[2:3], v4
s_endpgm

; ds_swizzle_b32 in 64 lanes. s[0:1]: the output address (256-byte aligned). Every lane swizzles
; i + 1 (never 0, so that a lane that reads 0 is told from one that reads a value). Stage k stores
; out[64k + i], and v[2:3] moves on by 0x100 bytes after each store. Stage 0 stores the number of
; lanes in the subgroup; stages 1 to 9 swizzle with a quad permutation (1 to 3) and the bit mask
; mode (4 to 9). In stages 10 and 11 EXEC has the lanes 0 to 2 of each quad set, so the lane 3 of
; each quad is a source that reads 0, and the destination of that lane keeps 0xdead.
v_lshlrev_b32_e32 v1, 2, v0
v_or_b32_e32 v2, s0, v1             ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s1
v_add_i32_e32 v4, vcc, 1, v0        ; v4 = i + 1
s_bcnt1_i32_b32 s6, exec_lo         ; stage 0
s_bcnt1_i32_b32 s7, exec_hi
s_add_u32 s6, s6, s7
v_mov_b32_e32 v5, s6
flat_store_dword v[2:3], v5
v_add_i32_e32 v2, vcc, 0x100, v2
ds_swizzle_b32 v5, v4 offset:0x801b  ; stage 1
s_waitcnt lgkmcnt(0)
flat_store_dword v[2:3], v5
v_add_i32_e32 v2, vcc, 0x100, v2
ds_swizzle_b32 v5, v4 offset:0x80aa  ; stage 2
s_waitcnt lgkmcnt(0)
flat_store_dword v[2:3], v5
v_add_i32_e32 v2, vcc, 0x100, v2
ds_swizzle_b32 v5, v4 offset:0x8071  ; stage 3
s_waitcnt lgkmcnt(0)
flat_store_dword v[2:3], v5
v_add_i32_e32 v2, vcc, 0x100, v2
ds_swizzle_b32 v5, v4 offset:0x041f  ; stage 4
s_waitcnt lgkmcnt(0)
flat_store_dword v[2:3], v5
v_add_i32_e32 v2, vcc, 0x100, v2
ds_swizzle_b32 v5, v4 offset:0x401f  ; stage 5
s_waitcnt lgkmcnt(0)
flat_store_dword v[2:3], v5
v_add_i32_e32 v2, vcc, 0x100, v2
ds_swizzle_b32 v5, v4 offset:0x7c1f  ; stage 6
s_waitcnt lgkmcnt(0)
flat_store_dword v[2:3], v5
v_add_i32_e32 v2, vcc, 0x100, v2
ds_swizzle_b32 v5, v4 offset:0x0000  ; stage 7
s_waitcnt lgkmcnt(0)
flat_store_dword v[2:3], v5
v_add_i32_e32 v2, vcc, 0x100, v2
ds_swizzle_b32 v5, v4 offset:0x0103  ; stage 8
s_waitcnt lgkmcnt(0)
flat_store_dword v[2:3], v5
v_add_i32_e32 v2, vcc, 0x100, v2
ds_swizzle_b32 v5, v4 offset:0x0e0f  ; stage 9
s_waitcnt lgkmcnt(0)
flat_store_dword v[2:3], v5
v_add_i32_e32 v2, vcc, 0x100, v2
v_mov_b32_e32 v5, 0xdead
s_mov_b32 exec_lo, 0x77777777
s_mov_b32 exec_hi, 0x77777777
ds_swizzle_b32 v5, v4 offset:0x801b  ; stage 10
s_waitcnt lgkmcnt(0)
s_mov_b64 exec, -1
flat_store_dword v[2:3], v5
v_add_i32_e32 v2, vcc, 0x100, v2
v_mov_b32_e32 v5, 0xdead
s_mov_b32 exec_lo, 0x77777777
s_mov_b32 exec_hi, 0x77777777
ds_swizzle_b32 v5, v4 offset:0x041f  ; stage 11
s_waitcnt lgkmcnt(0)
s_mov_b64 exec, -1
flat_store_dword v[2:3], v5
v_add_i32_e32 v2, vcc, 0x100, v2
s_endpgm

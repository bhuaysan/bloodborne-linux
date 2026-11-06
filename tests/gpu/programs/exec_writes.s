; The ways to write EXEC that exec_lanes doesn't use. s[0:1]: the output address (512-byte
; aligned), s2 and s3: a lane mask, s4: 0x100. Lane i stores 0x100 | i to out[64 * k + i] under the
; EXEC of stage k, so a slot has a lane's value only if the lane was active.
v_lshlrev_b32_e32 v1, 2, v0
v_or_b32_e32 v2, s0, v1             ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s1
v_or_b32_e32 v4, 0x100, v2          ; v[4:5] = &out[64 + i]
v_mov_b32_e32 v5, s1
v_or_b32_e32 v6, 0x200, v2          ; v[6:7] = &out[128 + i]
v_mov_b32_e32 v7, s1
v_or_b32_e32 v8, 0x300, v2          ; v[8:9] = &out[192 + i]
v_mov_b32_e32 v9, s1
v_or_b32_e32 v10, 0x400, v2         ; v[10:11] = &out[256 + i]
v_mov_b32_e32 v11, s1
v_or_b32_e32 v12, 0x100, v0         ; the stored value
s_mov_b32 exec_lo, s2               ; stage 0: the mask, then EXEC to itself
s_mov_b32 exec_hi, s3
s_mov_b64 exec, exec
flat_store_dword v[2:3], v12
s_mov_b64 exec, 0                   ; stage 1: no lane
flat_store_dword v[4:5], v12
s_mov_b64 exec, -1                  ; stage 2: every lane
flat_store_dword v[6:7], v12
s_mov_b64 exec, 1.0                 ; stage 3: the f64 pattern, only the high dword is set
flat_store_dword v[8:9], v12
s_mov_b64 exec, -1                  ; stage 4: ALU on one half; the other keeps all its lanes
s_add_u32 exec_lo, s4, 0x11
flat_store_dword v[10:11], v12
s_endpgm

; EXEC predication with lane bits. s[0:1]: the output address (512-byte aligned), s2: a value,
; s3: the EXEC bits of lanes 0-31. Lane i writes out[i] and out[64 + i].
v_lshlrev_b32_e32 v1, 2, v0         ; v1 = 4 * i
v_or_b32_e32 v2, s0, v1             ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s1
v_or_b32_e32 v4, 0x100, v2          ; v[4:5] = &out[64 + i]
v_mov_b32_e32 v5, s1
v_xor_b32_e32 v6, s2, v1            ; every lane: v6 = s2 ^ 4i
s_mov_b32 exec_lo, s3               ; lane i < 32: bit i of s3
s_mov_b32 exec_hi, 0xf0f0f0f0       ; lane i >= 32: bit i - 32
v_and_b32_e32 v6, 0xffff, v6        ; active lanes only
flat_store_dword v[2:3], v6         ; active lanes only
s_mov_b64 exec, -4                  ; lanes 2-63
flat_store_dword v[4:5], v6
s_endpgm

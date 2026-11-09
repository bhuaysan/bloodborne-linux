; TGID_X_EN (bit 7 of COMPUTE_PGM_RSRC2): s5, after the five user SGPRs, is the workgroup's X.
; out[64X + i] = X << 8 | i, with s[0:1] the output address and each group 0x100 bytes on.
s_lshl_b32 s6, s5, 8
v_lshlrev_b32_e32 v1, 2, v0
v_or_b32_e32 v1, s6, v1
v_or_b32_e32 v2, s0, v1             ; v[2:3] = &out[64X + i]
v_mov_b32_e32 v3, s1
v_or_b32_e32 v4, s6, v0
flat_store_dword v[2:3], v4
s_endpgm

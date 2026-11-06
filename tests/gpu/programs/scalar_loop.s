; A loop and skips on SCC. s2 counts down to 0 while s5 sums it, and the loop ends when
; s_sub_u32 borrows. s3 + s3 carries, so the first skip is taken; s3 + 1 doesn't, so the second
; isn't. Lane i writes out[i] = (s5 + 64) ^ i, with VOP3 for an SGPR as the second source.
s_mov_b32 s5, 0
s_add_u32 s5, s5, s2                ; offset 4: the loop
s_sub_u32 s2, s2, 1                 ; SCC = borrow
s_cbranch_scc0 -3                   ; to offset 4 while s2 didn't borrow
s_add_u32 s6, s3, s3                ; SCC = carry
s_cbranch_scc1 1                    ; skips the next instruction
s_mov_b32 s5, 0
s_add_u32 s6, s3, 1                 ; SCC = 0: no carry
s_cbranch_scc1 1                    ; not taken
s_add_u32 s5, s5, 64
s_mov_b64 s[8:9], s[0:1]
v_lshlrev_b32_e32 v1, 2, v0
v_or_b32_e32 v2, s8, v1             ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s9
v_xor_b32_e64 v4, v0, s5
flat_store_dword v[2:3], v4
s_endpgm

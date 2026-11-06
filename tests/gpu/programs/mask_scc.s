; SCC after the SALU bitwise operations, with data that is the same in every lane. s[0:1]: the
; output address (512-byte aligned), s2 = s3 or not, s4: any value. Stage k stores out[64k + i]
; and v[2:3] moves on by 0x100 bytes after each store.
v_lshlrev_b32_e32 v1, 2, v0
v_or_b32_e32 v2, s0, v1             ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s1
v_mov_b32_e32 v4, s2
v_cmp_eq_u32_e64 s[6:7], s3, v4     ; all lanes if s2 == s3, none otherwise
s_mov_b32 s20, 0                    ; stage 0: one bit of s20 for each branch on SCC
s_and_b64 s[8:9], s[6:7], exec      ; any of the result: s2 == s3
s_cbranch_scc0 1
s_add_u32 s20, s20, 1
s_andn2_b64 s[8:9], s[6:7], s[6:7]  ; always 0, so SCC = 0 although the operands are set
s_cbranch_scc1 1
s_add_u32 s20, s20, 2
s_orn2_b64 s[8:9], s[6:7], s[6:7]   ; always all ones, so SCC = 1 although the operands are 0
s_cbranch_scc0 1
s_add_u32 s20, s20, 4
s_nor_b64 s[8:9], s[6:7], s[6:7]    ; the inverse: s2 != s3
s_cbranch_scc0 1
s_add_u32 s20, s20, 8
s_and_saveexec_b64 s[12:13], s[6:7] ; SCC = any of the new EXEC
s_cbranch_scc0 1
s_add_u32 s20, s20, 16
s_mov_b64 exec, s[12:13]
s_xor_saveexec_b64 s[14:15], s[6:7] ; EXEC = the inverse
s_cbranch_scc0 1
s_add_u32 s20, s20, 32
s_mov_b64 exec, s[14:15]
v_mov_b32_e32 v5, s20
flat_store_dword v[2:3], v5
v_add_i32_e32 v2, vcc, 0x100, v2
s_mov_b32 s22, 0                    ; stage 1: SCC = result != 0 on values
s_andn2_b32 s23, s2, s2             ; 0
s_cbranch_scc1 1
s_add_u32 s22, s22, 1
s_xnor_b32 s23, s2, s2              ; all ones
s_cbranch_scc0 1
s_add_u32 s22, s22, 2
v_mov_b32_e32 v5, s22
flat_store_dword v[2:3], v5
v_add_i32_e32 v2, vcc, 0x100, v2
s_and_b32 s21, s2, s3             ; stage 2: the eight operations in a chain on values
s_or_b32 s21, s21, s4
s_xor_b32 s21, s21, s2
s_andn2_b32 s21, s21, s3
s_orn2_b32 s21, s21, s4
s_nand_b32 s21, s21, s2
s_nor_b32 s21, s21, s3
s_xnor_b32 s21, s21, s4
v_mov_b32_e32 v5, s21
flat_store_dword v[2:3], v5
s_endpgm

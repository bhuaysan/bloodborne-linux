; The 12 s_cmp_<cond>_<i32|u32> compares, one SCC bit each. s[0:1]: the output address (512-byte
; aligned), s2: -2, s3: 1. Stage 0 compares s2 with s3 (-2 against 1: signed less, unsigned
; greater), stage 1 s3 with itself. Stage k stores out[64k + i]; v[2:3] moves on by 0x100 bytes
; after each store. Compare n (eq, lg, gt, ge, lt, le on i32, then on u32) is bit n of the result;
; a bit above 64 is a literal of two words, so its branch skips 2.
v_lshlrev_b32_e32 v1, 2, v0
v_or_b32_e32 v2, s0, v1             ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s1
s_mov_b32 s20, 0                    ; stage 0
s_cmp_eq_i32 s2, s3
s_cbranch_scc0 1
s_add_u32 s20, s20, 1
s_cmp_lg_i32 s2, s3
s_cbranch_scc0 1
s_add_u32 s20, s20, 2
s_cmp_gt_i32 s2, s3
s_cbranch_scc0 1
s_add_u32 s20, s20, 4
s_cmp_ge_i32 s2, s3
s_cbranch_scc0 1
s_add_u32 s20, s20, 8
s_cmp_lt_i32 s2, s3
s_cbranch_scc0 1
s_add_u32 s20, s20, 16
s_cmp_le_i32 s2, s3
s_cbranch_scc0 1
s_add_u32 s20, s20, 32
s_cmp_eq_u32 s2, s3
s_cbranch_scc0 1
s_add_u32 s20, s20, 64
s_cmp_lg_u32 s2, s3
s_cbranch_scc0 2
s_add_u32 s20, s20, 128
s_cmp_gt_u32 s2, s3
s_cbranch_scc0 2
s_add_u32 s20, s20, 256
s_cmp_ge_u32 s2, s3
s_cbranch_scc0 2
s_add_u32 s20, s20, 512
s_cmp_lt_u32 s2, s3
s_cbranch_scc0 2
s_add_u32 s20, s20, 1024
s_cmp_le_u32 s2, s3
s_cbranch_scc0 2
s_add_u32 s20, s20, 2048
v_mov_b32_e32 v5, s20
flat_store_dword v[2:3], v5
v_add_i32_e32 v2, vcc, 0x100, v2
s_mov_b32 s21, 0                    ; stage 1
s_cmp_eq_i32 s3, s3
s_cbranch_scc0 1
s_add_u32 s21, s21, 1
s_cmp_lg_i32 s3, s3
s_cbranch_scc0 1
s_add_u32 s21, s21, 2
s_cmp_gt_i32 s3, s3
s_cbranch_scc0 1
s_add_u32 s21, s21, 4
s_cmp_ge_i32 s3, s3
s_cbranch_scc0 1
s_add_u32 s21, s21, 8
s_cmp_lt_i32 s3, s3
s_cbranch_scc0 1
s_add_u32 s21, s21, 16
s_cmp_le_i32 s3, s3
s_cbranch_scc0 1
s_add_u32 s21, s21, 32
s_cmp_eq_u32 s3, s3
s_cbranch_scc0 1
s_add_u32 s21, s21, 64
s_cmp_lg_u32 s3, s3
s_cbranch_scc0 2
s_add_u32 s21, s21, 128
s_cmp_gt_u32 s3, s3
s_cbranch_scc0 2
s_add_u32 s21, s21, 256
s_cmp_ge_u32 s3, s3
s_cbranch_scc0 2
s_add_u32 s21, s21, 512
s_cmp_lt_u32 s3, s3
s_cbranch_scc0 2
s_add_u32 s21, s21, 1024
s_cmp_le_u32 s3, s3
s_cbranch_scc0 2
s_add_u32 s21, s21, 2048
v_mov_b32_e32 v5, s21
flat_store_dword v[2:3], v5
s_endpgm

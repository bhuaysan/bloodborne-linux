; The 16 float compare conditions on x = i - 32, a NaN in every lane with i % 8 = 7, against 0.
; s[0:1]: the output address (512-byte aligned). Stage 0 stores one result bit per condition (f,
; lt, eq, le, gt, lg, ge, o, u, nge, nlg, ngt, nle, neq, nlt, tru, the first one highest), stage 1
; the VOP3 form with modifiers, and stage 2 is v_cmpx, which leaves only some lanes to store.
v_lshlrev_b32_e32 v1, 2, v0
v_or_b32_e32 v2, s0, v1             ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s1
v_cvt_f32_u32_e32 v4, v0
v_subrev_f32_e32 v5, 0x42000000, v4 ; v5 = x
v_and_b32_e32 v8, 7, v0
v_cmp_eq_u32_e32 vcc, 7, v8
v_mov_b32_e32 v11, 0x7fc00000
v_cndmask_b32_e32 v9, v5, v11, vcc  ; v9 = x, or NaN
v_mov_b32_e32 v7, 0
v_cmp_f_f32_e32 vcc, 0, v9
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_lt_f32_e32 vcc, 0, v9
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_eq_f32_e32 vcc, 0, v9
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_le_f32_e32 vcc, 0, v9
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_gt_f32_e32 vcc, 0, v9
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_lg_f32_e32 vcc, 0, v9
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_ge_f32_e32 vcc, 0, v9
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_o_f32_e32 vcc, 0, v9
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_u_f32_e32 vcc, 0, v9
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_nge_f32_e32 vcc, 0, v9
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_nlg_f32_e32 vcc, 0, v9
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_ngt_f32_e32 vcc, 0, v9
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_nle_f32_e32 vcc, 0, v9
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_neq_f32_e32 vcc, 0, v9
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_nlt_f32_e32 vcc, 0, v9
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_tru_f32_e32 vcc, 0, v9
v_addc_u32_e32 v7, vcc, v7, v7, vcc
flat_store_dword v[2:3], v7
v_add_i32_e32 v2, vcc, 0x100, v2
v_cmp_lt_f32_e64 s[6:7], -|v9|, -1.0 ; stage 1: -|x| < -1 (false for NaN)
v_cndmask_b32_e64 v6, 0, 1, s[6:7]
flat_store_dword v[2:3], v6
v_add_i32_e32 v2, vcc, 0x100, v2
v_cmpx_gt_f32_e32 vcc, 0, v9        ; stage 2: EXEC = lanes with x < 0
v_mov_b32_e32 v6, 5
flat_store_dword v[2:3], v6
s_endpgm

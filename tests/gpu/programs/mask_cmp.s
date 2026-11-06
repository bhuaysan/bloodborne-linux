; The 16 integer compare conditions. s[0:1]: the output address (512-byte aligned), s2: -16.
; Lane i compares s2 with i - 32 (src0 is the left operand) and gets one result bit per condition,
; the i32 conditions f, lt, eq, le, gt, ne, ge, t, then the u32 ones, the first one highest.
v_lshlrev_b32_e32 v1, 2, v0
v_or_b32_e32 v2, s0, v1             ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s1
v_subrev_i32_e32 v1, vcc, 32, v0    ; v1 = i - 32
v_mov_b32_e32 v7, 0
v_cmp_f_i32_e32 vcc, s2, v1
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_lt_i32_e32 vcc, s2, v1
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_eq_i32_e32 vcc, s2, v1
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_le_i32_e32 vcc, s2, v1
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_gt_i32_e32 vcc, s2, v1
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_ne_i32_e32 vcc, s2, v1
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_ge_i32_e32 vcc, s2, v1
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_t_i32_e32 vcc, s2, v1
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_f_u32_e32 vcc, s2, v1
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_lt_u32_e32 vcc, s2, v1
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_eq_u32_e32 vcc, s2, v1
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_le_u32_e32 vcc, s2, v1
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_gt_u32_e32 vcc, s2, v1
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_ne_u32_e32 vcc, s2, v1
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_ge_u32_e32 vcc, s2, v1
v_addc_u32_e32 v7, vcc, v7, v7, vcc
v_cmp_t_u32_e32 vcc, s2, v1
v_addc_u32_e32 v7, vcc, v7, v7, vcc
flat_store_dword v[2:3], v7
s_endpgm

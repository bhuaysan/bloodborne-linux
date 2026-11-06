; f32 edge cases: NaN operands, infinities, products that a fused multiply-add would round
; differently, and the rounding mode. s[0:1]: the output address (512-byte aligned).
; a = x = i - 32, or a NaN in the odd lanes (v5); b = 2.5, or a NaN where bit 1 of i is set (v6);
; c = +inf, -inf, NaN or 3e9 for i % 4 = 0 to 3 (v10). Stage k stores out[64k + i], and v[2:3]
; moves on by 0x100 bytes after each store.
.macro put r
flat_store_dword v[2:3], \r
v_add_i32_e32 v2, vcc, 0x100, v2
.endm
v_lshlrev_b32_e32 v1, 2, v0
v_or_b32_e32 v2, s0, v1             ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s1
v_cvt_f32_u32_e32 v4, v0
v_subrev_f32_e32 v8, 0x42000000, v4 ; v8 = x
v_mov_b32_e32 v11, 0x7fc00000       ; NaN
v_mov_b32_e32 v12, 0x40200000       ; 2.5
v_mov_b32_e32 v13, 0xff800000       ; -infinity
v_mov_b32_e32 v14, 0x4f32d05e       ; 3e9
v_mov_b32_e32 v15, 0x3f800800       ; 1 + 2^-12
v_mov_b32_e32 v16, 0xbf801000       ; -(1 + 2^-11)
v_mov_b32_e32 v17, 0
v_mov_b32_e32 v18, 0x02000003
v_mov_b32_e32 v19, 0x33c00000       ; 1.5 * 2^-24
v_and_b32_e32 v9, 1, v0
v_cmp_eq_u32_e32 vcc, 1, v9
v_cndmask_b32_e32 v5, v8, v11, vcc  ; v5 = a
v_and_b32_e32 v9, 2, v0
v_cmp_eq_u32_e32 vcc, 2, v9
v_cndmask_b32_e32 v6, v12, v11, vcc ; v6 = b
v_mov_b32_e32 v10, 0x7f800000
v_and_b32_e32 v9, 3, v0
v_cmp_eq_u32_e32 vcc, 1, v9
v_cndmask_b32_e32 v10, v10, v13, vcc
v_cmp_eq_u32_e32 vcc, 2, v9
v_cndmask_b32_e32 v10, v10, v11, vcc
v_cmp_eq_u32_e32 vcc, 3, v9
v_cndmask_b32_e32 v10, v10, v14, vcc ; v10 = c
v_min_f32_e32 v7, v5, v6            ; 0: a NaN operand gives the other one
put v7
v_max_f32_e32 v7, v5, v6            ; 1
put v7
v_add_f32_e64 v7, v5, 0 clamp       ; 2: clamp of a, a NaN gives 0 only with DX10_CLAMP
put v7
v_cvt_i32_f32_e32 v7, v10           ; 3: saturates on infinities, a NaN gives 0
put v7
v_cvt_u32_f32_e32 v7, v10           ; 4
put v7
v_mad_legacy_f32 v7, 0, v10, 1.0    ; 5: 0 * c is 0, so 1
put v7
v_mad_f32 v7, 0, v10, 1.0           ; 6: 0 * c is NaN for an infinity or NaN
put v7
v_mov_b32_e32 v7, 2.0
v_mac_legacy_f32_e32 v7, v10, v17   ; 7: 2 + c * 0 with the legacy rule, 2
put v7
v_mad_f32 v7, v15, v15, v16         ; 8: the product rounds first, so 0
put v7
v_fma_f32 v7, v15, v15, v16         ; 9: fused, 2^-24
put v7
v_mov_b32_e32 v7, v16
v_mac_f32_e32 v7, v15, v15          ; 10: not fused, 0
put v7
v_add_f32_e32 v7, 1.0, v19          ; 11: 1 + 0.75 ulp: up with round to nearest, down toward zero
put v7
v_cvt_f32_u32_e32 v7, v18           ; 12: 2^25 + 3, the same rounding
put v7
s_endpgm

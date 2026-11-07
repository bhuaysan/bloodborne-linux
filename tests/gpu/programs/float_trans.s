; f32 transcendentals and the half float conversions. s[0:1]: the output address (512-byte
; aligned). x = (i - 32) * 0.375 (v5), but 1 in lane 0 and -0, +inf, -inf, NaN, 1e20, -256,
; 256.5 and 1e-30 in lanes 56 to 63; y = 7777.75 * x (v8), z = 3.0e-6 * x (v9), and h = 0xabcd0000 | i * 0x405 (v10:
; a half of exponent i % 32, sign i / 32 and mantissa 5 * i). Stage k stores out[64k + i], and
; v[2:3] moves on by 0x100 bytes after each store.
.macro put r
flat_store_dword v[2:3], \r
v_add_i32_e32 v2, vcc, 0x100, v2
.endm
.macro special lane, value
v_mov_b32_e32 v11, \value
v_cmp_ne_u32_e32 vcc, \lane, v0
v_cndmask_b32_e32 v5, v11, v5, vcc
.endm
v_lshlrev_b32_e32 v1, 2, v0
v_or_b32_e32 v2, s0, v1             ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s1
v_cvt_f32_u32_e32 v4, v0
v_subrev_f32_e32 v5, 0x42000000, v4
v_mul_f32_e32 v5, 0x3ec00000, v5
special 0, 1.0
special 56, 0x80000000
special 57, 0x7f800000
special 58, 0xff800000
special 59, 0x7fc00000
special 60, 0x60ad78ec
special 61, 0xc3800000
special 62, 0x43804000
special 63, 0x0da24260
v_mul_f32_e32 v8, 0x45f30e00, v5
v_mul_f32_e32 v9, 0x3649539c, v5
v_mul_u32_u24_e32 v10, 0x405, v0
v_or_b32_e32 v10, 0xabcd0000, v10
v_rcp_f32_e32 v7, v5                ; 0
put v7
v_rcp_iflag_f32_e32 v7, v5          ; 1
put v7
v_rcp_clamp_f32_e32 v7, v5          ; 2
put v7
v_rcp_legacy_f32_e32 v7, v5         ; 3
put v7
v_rsq_f32_e32 v7, v5                ; 4
put v7
v_rsq_clamp_f32_e32 v7, v5          ; 5
put v7
v_sqrt_f32_e32 v7, v5               ; 6
put v7
v_exp_f32_e32 v7, v5                ; 7
put v7
v_log_f32_e32 v7, v5                ; 8
put v7
v_sin_f32_e32 v7, v5                ; 9
put v7
v_cos_f32_e32 v7, v5                ; 10
put v7
v_rcp_f32_e64 v7, -|v5|             ; 11
put v7
v_exp_f32_e64 v7, -v5 clamp         ; 12
put v7
v_log_f32_e64 v7, |v5| mul:2        ; 13
put v7
v_sqrt_f32_e64 v7, |v5| div:2       ; 14
put v7
v_cvt_pkrtz_f16_f32_e32 v7, v9, v8  ; 15: z in the low half
put v7
v_cvt_pkrtz_f16_f32_e64 v7, -v8, |v5| ; 16
put v7
v_cvt_f32_f16_e32 v7, v10           ; 17
put v7
v_cvt_f32_f16_e64 v7, -|v10|        ; 18
put v7
v_mul_f32_e32 v12, 0x41800000, v5   ; 16 * x, past the overflow and underflow of exp
v_exp_f32_e32 v7, v12               ; 19
put v7
s_endpgm

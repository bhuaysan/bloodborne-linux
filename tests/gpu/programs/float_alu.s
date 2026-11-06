; f32 VALU arithmetic, modifiers and conversions. s[0:1]: the output address (512-byte aligned).
; x = i - 32 as a float (v5), y = 0.75 * x (v8), z = 1e10 * x (v7). Stage k stores out[64k + i],
; and v[2:3] moves on by 0x100 bytes after each store.
.macro put r
flat_store_dword v[2:3], \r
v_add_i32_e32 v2, vcc, 0x100, v2
.endm
v_lshlrev_b32_e32 v1, 2, v0
v_or_b32_e32 v2, s0, v1             ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s1
v_cvt_f32_u32_e32 v4, v0
v_subrev_f32_e32 v5, 0x42000000, v4 ; v5 = x
v_mul_f32_e32 v8, 0x3f400000, v5    ; v8 = y
v_mul_f32_e32 v7, 0x501502f9, v5    ; v7 = z
v_mov_b32_e32 v9, 0x7f800000        ; +infinity
v_add_f32_e32 v6, 0x3f400000, v5    ; 0: x + 0.75
put v6
v_sub_f32_e32 v6, 0x3f400000, v5    ; 1: 0.75 - x
put v6
v_subrev_f32_e32 v6, 0x3f400000, v5 ; 2: x - 0.75
put v6
v_mad_f32 v6, v5, v8, v5            ; 3: x * y + x
put v6
v_fma_f32 v6, v5, v8, 1.0           ; 4: fma(x, y, 1)
put v6
v_max_f32_e32 v6, 0x3f400000, v5    ; 5: max(0.75, x)
put v6
v_min_f32_e32 v6, 0, v5             ; 6: min(0, x)
put v6
v_mul_f32_e64 v6, -|v5|, v8         ; 7: -|x| * y
put v6
v_mul_f32_e64 v6, v5, v8 clamp      ; 8: clamp(x * y)
put v6
v_mul_f32_e64 v6, v5, v8 clamp mul:4 ; 9: omod, then clamp
put v6
v_mul_f32_e64 v6, v5, v8 div:2      ; 10: x * y / 2
put v6
v_cvt_i32_f32_e32 v6, v8            ; 11: y toward zero
put v6
v_cvt_u32_f32_e32 v6, v8            ; 12: negative y gives 0
put v6
v_cvt_i32_f32_e32 v6, v7            ; 13: saturates
put v6
v_cvt_u32_f32_e64 v6, |v7|          ; 14: saturates
put v6
v_cvt_flr_i32_f32_e32 v6, v8        ; 15
put v6
v_cvt_rpi_i32_f32_e32 v6, v8        ; 16: floor(y + 0.5)
put v6
v_fract_f32_e32 v6, v8              ; 17
put v6
v_floor_f32_e32 v6, v8              ; 18
put v6
v_ceil_f32_e32 v6, v8               ; 19
put v6
v_trunc_f32_e32 v6, v8              ; 20
put v6
v_rndne_f32_e32 v6, v8              ; 21
put v6
v_cvt_f32_i32_e64 v6, v0 clamp      ; 22: i as a float, at most 1 (v6 is the VOP3 destination)
put v6
v_mul_legacy_f32_e32 v6, v9, v5     ; 23: infinity * x, 0 for x = 0
put v6
v_mul_f32_e32 v6, v9, v5            ; 24: infinity * x, NaN for x = 0
put v6
v_mov_b32_e32 v6, 2.0
v_mac_f32_e32 v6, 0x3f400000, v5    ; 25: 2 + 0.75 * x
put v6
v_madmk_f32 v6, v5, 0x3f400000, v8  ; 26: x * 0.75 + y
put v6
v_madak_f32 v6, v5, v8, 0x3f400000  ; 27: x * y + 0.75
put v6
v_min_legacy_f32_e32 v6, 0x3f400000, v5 ; 28
put v6
v_max_legacy_f32_e32 v6, 0x3f400000, v5 ; 29
put v6
v_fract_f32_e32 v6, 0xb2800000      ; 30: -2^-26, below 1 where x - floor(x) rounds to 1.0
put v6
s_endpgm

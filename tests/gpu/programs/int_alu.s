; The integer VALU: the 24- and 32-bit multiplies, bit fields, shifts, min, max, median and the
; 64-bit shifts. s[0:1]: the output address (512-byte aligned). With i the lane, a = 0x7fffffe0 + i
; (negative from lane 32), b = (i << 18) ^ 0x00fff0f1 (24-bit field negative below lane 32),
; c = i - 32, off = i % 32, width = i / 2 % 32, and v[12:13] = {a, a}. Stage k stores out[64k + i];
; v[2:3] moves on by 0x100 bytes after each store.
.macro put r
flat_store_dword v[2:3], \r
v_add_i32_e32 v2, vcc, 0x100, v2
.endm
v_lshlrev_b32_e32 v1, 2, v0
v_or_b32_e32 v2, s0, v1
v_mov_b32_e32 v3, s1
v_add_i32_e32 v4, vcc, 0x7fffffe0, v0
v_lshlrev_b32_e32 v5, 18, v0
v_xor_b32_e32 v5, 0x00fff0f1, v5
v_subrev_i32_e32 v6, vcc, 32, v0
v_and_b32_e32 v7, 31, v0
v_lshrrev_b32_e32 v8, 1, v0
v_and_b32_e32 v8, 31, v8
v_mov_b32_e32 v12, v4
v_mov_b32_e32 v13, v4
v_mul_lo_u32 v9, v4, v5             ; 0
put v9
v_mul_hi_u32 v9, v4, v5             ; 1
put v9
v_mul_lo_i32 v9, v4, v5             ; 2
put v9
v_mul_hi_i32 v9, v4, v5             ; 3
put v9
v_mul_u32_u24_e32 v9, v4, v5        ; 4
put v9
v_mul_hi_u32_u24_e32 v9, v4, v5     ; 5
put v9
v_mul_i32_i24_e32 v9, v4, v5        ; 6
put v9
v_mul_hi_i32_i24_e32 v9, v4, v5     ; 7
put v9
v_mad_u32_u24 v9, v4, v5, v6        ; 8
put v9
v_mad_i32_i24 v9, v4, v5, v6        ; 9
put v9
v_bfe_u32 v9, v4, v7, v8            ; 10
put v9
v_bfe_i32 v9, v4, v7, v8            ; 11
put v9
v_lshrrev_b32_e32 v9, v0, v4        ; 12: the shift count is taken modulo 32
put v9
v_ashrrev_i32_e32 v9, v0, v4        ; 13
put v9
v_lshl_b32_e32 v9, v4, v0           ; 14
put v9
v_lshr_b32_e32 v9, v4, v0           ; 15
put v9
v_ashr_i32_e32 v9, v4, v0           ; 16
put v9
v_min_i32_e32 v9, v4, v6            ; 17
put v9
v_max_u32_e32 v9, v4, v6            ; 18
put v9
v_min3_i32 v9, v4, v5, v6           ; 19
put v9
v_max3_u32 v9, v4, v5, v6           ; 20
put v9
v_med3_i32 v9, v4, v5, v6           ; 21
put v9
v_med3_u32 v9, v4, v5, v6           ; 22
put v9
v_lshl_b64 v[10:11], v[12:13], v0   ; 23: low dword, 24: high dword (shift modulo 64)
put v10
put v11
v_lshr_b64 v[10:11], v[12:13], v0   ; 25, 26
put v10
put v11
v_ashr_i64 v[10:11], v[12:13], v0   ; 27, 28
put v10
put v11
s_endpgm

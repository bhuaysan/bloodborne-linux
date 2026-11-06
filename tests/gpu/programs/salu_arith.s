; The SALU arithmetic, shifts, bit fields, SOPK forms and the mask operations s_not_b64 and
; s_wqm_b64. s[0:1]: the output address (512-byte aligned). Every lane stores the same scalar
; results. `scc` shifts the SCC of the instruction before it into s30 from the right. Stage k
; stores out[64k + i]; v[2:3] moves on by 0x100 bytes after each store.
.macro put r
flat_store_dword v[2:3], \r
v_add_i32_e32 v2, vcc, 0x100, v2
.endm
.macro val r
v_mov_b32_e32 v6, \r
put v6
.endm
.macro scc
s_cselect_b32 s31, 1, 0
s_lshl_b32 s30, s30, 1
s_or_b32 s30, s30, s31
.endm
v_lshlrev_b32_e32 v1, 2, v0
v_or_b32_e32 v2, s0, v1
v_mov_b32_e32 v3, s1
s_mov_b32 s10, 0x7fffffff
s_mov_b32 s11, 1
s_mov_b32 s12, 0xfffffff0           ; -16
s_mov_b32 s13, 5
s_mov_b32 s14, 35                   ; shift counts use the low 5 bits: 3
s_mov_b32 s15, 0x80008              ; bit field: offset 8, width 8
s_mov_b32 s16, 0x12f4a6b8
s_mov_b32 s30, 0
s_lshl_b32 s21, s16, s14            ; 0
scc
val s21
s_lshr_b32 s21, s12, s14            ; 1
scc
val s21
s_ashr_i32 s21, s12, s14            ; 2
scc
val s21
s_lshr_b32 s21, s11, 4              ; 3: a zero result
scc
val s21
s_bfe_u32 s21, s16, s15             ; 4
scc
val s21
s_bfe_i32 s21, s16, s15             ; 5: 0xa6 sign-extended
scc
val s21
s_bfe_i32 s21, s16, 0               ; 6: width 0
scc
val s21
s_movk_i32 s21, 0x8000              ; 7: sign-extended
val s21
s_mov_b32 s21, 0x7fffffff
s_addk_i32 s21, 1                   ; 8: overflow
scc
val s21
s_mov_b32 s21, 5
s_mulk_i32 s21, 0xfffd              ; 9: 5 * -3
val s21
s_mov_b32 s21, 0x8000
s_cmpk_eq_u32 s21, 0x8000           ; SCC only from here to the next result
scc
s_cmpk_lt_i32 s12, 0x8000           ; -16 < -32768
scc
s_cmpk_gt_u32 s12, 0x8000           ; 0xfffffff0 > 0x8000
scc
s_mov_b32 s21, 7
s_cmp_eq_u32 s11, 1
s_cmovk_i32 s21, 0x1234             ; 10: SCC is 1
val s21
s_cmp_eq_u32 s11, 0
s_mov_b32 s21, 7
s_cmovk_i32 s21, 0x1234             ; 11: SCC is 0
val s21
s_cmp_eq_u32 s11, 1
s_cselect_b32 s21, s10, s13         ; 12
val s21
s_cmp_eq_u32 s11, 0
s_cselect_b32 s21, s10, s13         ; 13
val s21
s_not_b32 s21, s11                  ; 14
scc
val s21
val s30                             ; 15: the SCC bits, the first one highest
s_mov_b32 s30, 0
s_add_i32 s21, s10, s11             ; 16: overflow
scc
val s21
s_sub_i32 s21, s12, s10             ; 17: overflow
scc
val s21
s_sub_i32 s21, s11, s13             ; 18: a borrow but no overflow
scc
val s21
s_min_i32 s21, s12, s13             ; 19: src0 is the minimum
scc
val s21
s_min_u32 s21, s12, s13             ; 20: src1 is
scc
val s21
s_max_i32 s21, s12, s13             ; 21
scc
val s21
s_max_u32 s21, s12, s13             ; 22
scc
val s21
s_mul_i32 s21, s12, s13             ; 23
val s21
val s30                             ; 24: the SCC bits of stages 16 to 22
v_and_b32_e32 v7, 15, v0
v_cmp_eq_u32_e64 s[6:7], 5, v7      ; lane 5 of each 16 (put changes vcc)
s_wqm_b64 s[8:9], s[6:7]
v_cndmask_b32_e64 v6, 0, 1, s[8:9]  ; 25: the quad of lanes 4-7 of each 16
put v6
s_not_b64 s[28:29], s[6:7]
v_cndmask_b32_e64 v6, 0, 1, s[28:29] ; 26
put v6
s_endpgm

; Cross-lane operations, over the subgroup in lane-local mode. s[0:1]: the output address
; (256-byte aligned), s[2:3]: a mask M, s4: a lane select above 63, s5: 0 (never written).
; Stage k stores out[64k + i], and v[2:3] moves on by 0x100 bytes after each store.
v_lshlrev_b32_e32 v1, 2, v0
v_or_b32_e32 v2, s0, v1             ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s1
v_or_b32_e32 v13, 0x100, v0         ; v13 = 0x100 | i
v_readfirstlane_b32 s6, v0          ; stage 0: the first lane of the subgroup
v_mov_b32_e32 v12, s6
flat_store_dword v[2:3], v12
v_add_i32_e32 v2, vcc, 0x100, v2
s_bcnt1_i32_b32 s6, exec_lo         ; stage 1: the lanes of the subgroup, from both halves
s_bcnt1_i32_b32 s7, exec_hi
s_add_u32 s6, s6, s7
v_mov_b32_e32 v12, s6
flat_store_dword v[2:3], v12
v_add_i32_e32 v2, vcc, 0x100, v2
v_mbcnt_lo_u32_b32_e64 v12, -1, 0   ; stage 2: the lane ID, without a ballot
v_mbcnt_hi_u32_b32_e64 v12, -1, v12
flat_store_dword v[2:3], v12
v_add_i32_e32 v2, vcc, 0x100, v2
s_mov_b32 s8, s2                    ; s[8:9] = M, a mask (it is copied to EXEC below)
s_mov_b32 s9, s3
v_mbcnt_lo_u32_b32_e64 v12, s8, 0   ; stage 3: the bits of M below the lane
v_mbcnt_hi_u32_b32_e64 v12, s9, v12
flat_store_dword v[2:3], v12
v_add_i32_e32 v2, vcc, 0x100, v2
s_bcnt0_i32_b64 s6, s[8:9]          ; stage 4
v_mov_b32_e32 v12, s6
flat_store_dword v[2:3], v12
v_add_i32_e32 v2, vcc, 0x100, v2
s_ff1_i32_b64 s6, s[8:9]            ; stage 5
v_mov_b32_e32 v12, s6
flat_store_dword v[2:3], v12
v_add_i32_e32 v2, vcc, 0x100, v2
s_ff0_i32_b64 s6, s[8:9]            ; stage 6
v_mov_b32_e32 v12, s6
flat_store_dword v[2:3], v12
v_add_i32_e32 v2, vcc, 0x100, v2
s_flbit_i32_b64 s6, s[8:9]          ; stage 7
v_mov_b32_e32 v12, s6
flat_store_dword v[2:3], v12
v_add_i32_e32 v2, vcc, 0x100, v2
s_ff1_i32_b32 s6, s9                ; stage 8
v_mov_b32_e32 v12, s6
flat_store_dword v[2:3], v12
v_add_i32_e32 v2, vcc, 0x100, v2
v_cmp_gt_u32_e32 vcc, 20, v0        ; stage 9: the lanes below 20 in vcc_lo, 0xdead if none
s_bcnt1_i32_b32 s6, vcc_lo
v_mov_b32_e32 v12, s6
s_cbranch_scc1 2
v_mov_b32_e32 v12, 0xdead
flat_store_dword v[2:3], v12
v_add_i32_e32 v2, vcc, 0x100, v2
s_flbit_i32 s6, s3                  ; stage 10: signed, on values
s_flbit_i32_i64 s7, s[4:5]
v_mov_b32_e32 v12, s7
v_lshlrev_b32_e32 v12, 8, v12
v_or_b32_e32 v12, s6, v12
flat_store_dword v[2:3], v12
v_add_i32_e32 v2, vcc, 0x100, v2
v_readlane_b32 s6, v13, s4          ; stage 11: v13 of lane s4 modulo 64
v_mov_b32_e32 v12, s6
flat_store_dword v[2:3], v12
v_add_i32_e32 v2, vcc, 0x100, v2
s_mov_b64 exec, s[8:9]              ; under EXEC = M: stage 12 reads the first lane, stage 13
v_readfirstlane_b32 s6, v13         ; writes 7 into lane s4 modulo 64, which M leaves out
v_writelane_b32 v13, 7, s4
s_mov_b64 exec, -1
v_mov_b32_e32 v12, s6
flat_store_dword v[2:3], v12
v_add_i32_e32 v2, vcc, 0x100, v2
flat_store_dword v[2:3], v13
s_endpgm

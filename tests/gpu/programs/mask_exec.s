; Lane masks in the SALU. s[0:1]: the output address (512-byte aligned). Stage k stores
; out[64k + i], and v[2:3] moves on by 0x100 bytes after each store.
v_lshlrev_b32_e32 v1, 2, v0
v_or_b32_e32 v2, s0, v1             ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s1
v_mov_b32_e32 v30, 0                ; stage 0: nested predication with s_and_saveexec_b64
v_cmp_gt_u32_e32 vcc, 32, v0
s_and_saveexec_b64 s[12:13], vcc    ; EXEC = lanes 0-31
v_mov_b32_e32 v30, 1
v_cmp_gt_u32_e32 vcc, 8, v0
s_and_saveexec_b64 s[14:15], vcc    ; EXEC = lanes 0-7
v_mov_b32_e32 v30, 2
s_mov_b64 exec, s[14:15]            ; lanes 0-31
v_or_b32_e32 v30, 0x10, v30
s_mov_b64 exec, s[12:13]            ; every lane
v_cmpx_gt_u32_e32 vcc, 4, v0        ; EXEC = lanes 0-3
v_or_b32_e32 v30, 0x20, v30
s_mov_b64 exec, -1
flat_store_dword v[2:3], v30
v_add_i32_e32 v2, vcc, 0x100, v2
v_and_b32_e32 v4, 1, v0             ; stage 1: the eight bitwise operations on A = bit 0 of the
v_cmp_eq_u32_e64 s[6:7], 1, v4      ; lane and B = bit 1, one result bit each, first one highest
v_and_b32_e32 v4, 2, v0
v_cmp_eq_u32_e64 s[8:9], 2, v4
v_mov_b32_e32 v7, 0
s_and_b64 vcc, s[6:7], s[8:9]
v_addc_u32_e32 v7, vcc, v7, v7, vcc
s_or_b64 vcc, s[6:7], s[8:9]
v_addc_u32_e32 v7, vcc, v7, v7, vcc
s_xor_b64 vcc, s[6:7], s[8:9]
v_addc_u32_e32 v7, vcc, v7, v7, vcc
s_andn2_b64 vcc, s[6:7], s[8:9]
v_addc_u32_e32 v7, vcc, v7, v7, vcc
s_orn2_b64 vcc, s[6:7], s[8:9]
v_addc_u32_e32 v7, vcc, v7, v7, vcc
s_nand_b64 vcc, s[6:7], s[8:9]
v_addc_u32_e32 v7, vcc, v7, v7, vcc
s_nor_b64 vcc, s[6:7], s[8:9]
v_addc_u32_e32 v7, vcc, v7, v7, vcc
s_xnor_b64 vcc, s[6:7], s[8:9]
v_addc_u32_e32 v7, vcc, v7, v7, vcc
flat_store_dword v[2:3], v7
s_endpgm

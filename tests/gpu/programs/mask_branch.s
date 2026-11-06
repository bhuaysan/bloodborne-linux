; An EXEC loop and the branches on VCC, SCC and EXEC. s[0:1]: the output address (512-byte
; aligned), s2: -2, s3: 1. Stage k stores out[64k + i], and v[2:3] moves on by 0x100 bytes after
; each store.
v_lshlrev_b32_e32 v1, 2, v0
v_or_b32_e32 v2, s0, v1             ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s1
v_and_b32_e32 v21, 7, v0            ; stage 0: a loop that each lane leaves after i & 7 rounds
v_mov_b32_e32 v22, 0
s_mov_b64 s[10:11], exec
v_cmp_lt_u32_e32 vcc, v22, v21      ; the loop
s_and_b64 exec, exec, vcc
s_cbranch_execz 2
v_add_i32_e32 v22, vcc, 1, v22
s_branch -5
s_mov_b64 exec, s[10:11]
flat_store_dword v[2:3], v22
v_add_i32_e32 v2, vcc, 0x100, v2
v_mov_b32_e32 v10, 0                ; stage 1: branches on VCC, SCC and EXEC. Lane 3 of every 8
v_and_b32_e32 v4, 7, v0             ; has the bit, so every subgroup of 8 or more agrees
v_cmp_eq_u32_e32 vcc, 3, v4
s_cbranch_vccz 1                    ; not taken
v_or_b32_e32 v10, 1, v10
v_cmp_lt_u32_e32 vcc, 7, v4         ; no lane
s_cbranch_vccz 1                    ; taken
v_or_b32_e32 v10, 2, v10            ; skipped
s_cbranch_vccnz 1                   ; not taken
v_or_b32_e32 v10, 4, v10
v_cmp_eq_u32_e32 vcc, 3, v4
s_cbranch_vccnz 1                   ; taken
v_or_b32_e32 v10, 8, v10            ; skipped
s_cmp_lt_i32 s2, s3                 ; -2 < 1
s_cbranch_scc0 1                    ; not taken
v_or_b32_e32 v10, 16, v10
s_cmp_lt_u32 s2, s3                 ; 0xfffffffe < 1 is false
s_cbranch_scc1 1                    ; not taken
v_or_b32_e32 v10, 32, v10
s_and_b64 exec, exec, vcc           ; lanes 3 of 8
s_cbranch_execnz 1                  ; taken
v_or_b32_e32 v10, 64, v10           ; skipped
s_mov_b64 exec, -1
flat_store_dword v[2:3], v10
s_endpgm

; Lane masks in user data pairs after their last read: s[0:1] holds the output address, s2 and s4
; hold inputs, s5 is free. Stage k stores out[64k + i], and v[2:3] moves on by 0x100 bytes.
v_lshlrev_b32_e32 v1, 2, v0
v_or_b32_e32 v2, s0, v1             ; v[2:3] = &out[i], the last reads of s[0:1]
v_mov_b32_e32 v3, s1
v_mov_b32_e32 v10, s2               ; the input in s2, read before a mask overwrites it
v_mov_b32_e32 v11, s4               ; the input in s4
s_mov_b64 s[0:1], exec              ; every lane, saved in the first user pair
v_cmp_gt_u32_e64 s[2:3], 8, v0      ; lanes 0-7 in another one
v_mov_b32_e32 v4, 1                 ; stage 0: predicated by the mask in s[2:3], then restored
s_mov_b64 exec, s[2:3]
v_mov_b32_e32 v4, 2
s_mov_b64 exec, s[0:1]
flat_store_dword v[2:3], v4
v_add_i32_e32 v2, vcc, 0x100, v2
v_mov_b32_e32 v5, s2                ; stages 1 and 2: the mask's dwords read as values
v_mov_b32_e32 v6, s3
flat_store_dword v[2:3], v5
v_add_i32_e32 v2, vcc, 0x100, v2
flat_store_dword v[2:3], v6
v_add_i32_e32 v2, vcc, 0x100, v2
v_mov_b32_e32 v7, 0                 ; stage 3: a saveexec into s[4:5], which starts as user data
s_and_saveexec_b64 s[4:5], s[2:3]
v_mov_b32_e32 v7, 3
s_mov_b64 exec, s[4:5]
flat_store_dword v[2:3], v7
v_add_i32_e32 v2, vcc, 0x100, v2
flat_store_dword v[2:3], v10        ; stage 4: the input of s2
v_add_i32_e32 v2, vcc, 0x100, v2
flat_store_dword v[2:3], v11        ; stage 5: the input of s4
v_add_i32_e32 v2, vcc, 0x100, v2
s_mov_b32 s2, 0x55                  ; stage 6: a value written over half of a mask reads back
v_mov_b32_e32 v12, s2
flat_store_dword v[2:3], v12
s_endpgm

; LDS reads and writes, in 128 lanes (two waves). s[0:1]: the output address (512-byte aligned).
; Stage k stores out[128k + i], and v[2:3] moves on by 0x200 bytes after each store.
s_mov_b32 m0, -1
v_lshlrev_b32_e32 v1, 2, v0         ; v1 = 4i
v_or_b32_e32 v2, s0, v1             ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s1
v_add_i32_e32 v4, vcc, v1, v0
v_add_i32_e32 v4, vcc, 7, v4        ; v4 = 5i + 7
ds_write_b32 v1, v4                 ; stage 0: LDS[4i], then the barrier, then the other wave's
s_waitcnt lgkmcnt(0)                ; lane 127 - i
s_barrier
v_sub_i32_e32 v5, vcc, 0x7f, v0
v_lshlrev_b32_e32 v5, 2, v5
ds_read_b32 v6, v5
s_waitcnt lgkmcnt(0)
flat_store_dword v[2:3], v6
v_add_i32_e32 v2, vcc, 0x200, v2
v_lshlrev_b32_e32 v7, 3, v0         ; stages 1-4: two dwords at 0x800 + 8i
v_add_i32_e32 v7, vcc, 0x800, v7
ds_write2_b32 v7, v4, v0 offset1:1  ; 5i + 7, then i
ds_read_b64 v[10:11], v7
ds_read2_b32 v[12:13], v7 offset0:1 ; the two dwords in reverse order
s_waitcnt lgkmcnt(0)
flat_store_dword v[2:3], v10
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v11
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v12
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v13
v_add_i32_e32 v2, vcc, 0x200, v2
v_add_i32_e32 v8, vcc, 0x1000, v1   ; stage 5: the st64 forms step 256 bytes per offset unit
ds_write2st64_b32 v8, v4, v0 offset1:4
ds_read2st64_b32 v[14:15], v8 offset1:4
s_waitcnt lgkmcnt(0)
flat_store_dword v[2:3], v14
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v15
v_add_i32_e32 v2, vcc, 0x200, v2
v_lshlrev_b32_e32 v9, 3, v0         ; stages 6-7: the 64-bit pairs, at 0x2000 + 8i and 0x2800 + 8i
v_add_i32_e32 v10, vcc, 0x2000, v9  ; (the second write2 covers the last 2 KB of the LDS)
v_add_i32_e32 v9, vcc, 0x2800, v9
v_mov_b32_e32 v16, v4               ; v[16:17] = (5i + 7, i), v[18:19] = (5i + 0x107, 2i)
v_mov_b32_e32 v17, v0
v_add_i32_e32 v18, vcc, 0x100, v4
v_lshlrev_b32_e32 v19, 1, v0
ds_write2st64_b64 v10, v[16:17], v[18:19] offset1:2  ; the second pair is 1024 bytes on
ds_read2_b64 v[20:23], v10 offset1:128               ; 128 * 8 bytes
ds_write2_b64 v9, v[18:19], v[16:17] offset1:128     ; the pairs the other way round
ds_read2st64_b64 v[24:27], v9 offset1:2
s_waitcnt lgkmcnt(0)
flat_store_dword v[2:3], v20
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v21
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v22
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v23
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v24
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v25
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v26
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v27
v_add_i32_e32 v2, vcc, 0x200, v2
v_lshlrev_b32_e32 v11, 4, v0        ; stage 8: b96 and b64 writes, 16 bytes per lane at 0x1800
v_add_i32_e32 v11, vcc, 0x1800, v11 ; (b96 accesses need 16-byte alignment on this hardware)
v_add_i32_e32 v20, vcc, 0x2000, v0
v_add_i32_e32 v21, vcc, 0x3000, v0
ds_write_b96 v11, v[16:18]          ; (5i + 7, i, 5i + 0x107)
ds_write_b64 v11, v[20:21] offset:8 ; replaces the third dword, adds the fourth
ds_read_b96 v[28:30], v11           ; 5i + 7, i, i + 0x2000
ds_read_b32 v31, v11 offset:12      ; i + 0x3000
s_waitcnt lgkmcnt(0)
flat_store_dword v[2:3], v28
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v29
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v30
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v31
s_endpgm

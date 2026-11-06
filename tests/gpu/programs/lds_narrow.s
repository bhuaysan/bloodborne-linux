; Sub-dword and 64 to 128-bit LDS accesses, accesses outside the LDS (12288 bytes), and bytes and
; halfwords that many lanes write at once, in 128 lanes. s[0:1]: the output address (512-byte
; aligned). Stage k stores out[128k + i], and v[2:3] moves on by 0x200 bytes after each store.
s_mov_b32 m0, -1
v_lshlrev_b32_e32 v1, 2, v0
v_or_b32_e32 v2, s0, v1             ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s1
v_mov_b32_e32 v30, 0
ds_write_b32 v1, v30                ; dwords 0-127 = 0
v_add_i32_e32 v4, vcc, 0x1800, v0   ; the byte 6144 + i holds (i + 1) | 0x80
v_add_i32_e32 v5, vcc, 1, v0
v_or_b32_e32 v5, 0x80, v5
ds_write_b8 v4, v5
v_lshlrev_b32_e32 v6, 1, v0         ; the halfword 6400 + 2i holds the byte twice
v_add_i32_e32 v6, vcc, 0x1900, v6
v_lshlrev_b32_e32 v7, 8, v5
v_or_b32_e32 v7, v7, v5
ds_write_b16 v6, v7
v_lshlrev_b32_e32 v13, 4, v0        ; the 16 bytes at 8192 + 16i hold i to i + 3
v_add_i32_e32 v13, vcc, 0x2000, v13
v_add_i32_e32 v17, vcc, 1, v0
v_add_i32_e32 v18, vcc, 2, v0
v_add_i32_e32 v19, vcc, 3, v0
v_mov_b32_e32 v16, v0
ds_write_b128 v13, v[16:19]
v_and_b32_e32 v31, 3, v0            ; the byte 6656 + (i % 4) is written by 32 lanes, each with
v_add_i32_e32 v32, vcc, 0x1a00, v31 ; its own bit: 1 << (i % 8)
v_mov_b32_e32 v35, 1
v_and_b32_e32 v33, 7, v0
v_lshlrev_b32_e32 v33, v33, v35
ds_write_b8 v32, v33
v_and_b32_e32 v34, 1, v0            ; the halfword 6672 + 2 * (i % 2) by 64 lanes with 1 << (i % 16)
v_lshlrev_b32_e32 v34, 1, v34
v_add_i32_e32 v34, vcc, 0x1a10, v34
v_and_b32_e32 v36, 15, v0
v_lshlrev_b32_e32 v36, v36, v35
ds_write_b16 v34, v36
v_mov_b32_e32 v27, 0x3000           ; the first byte past the LDS
v_mov_b32_e32 v28, 0xdead
ds_write_b32 v27, v28               ; dropped
v_mov_b32_e32 v26, 7
ds_read_b32 v26, v27                ; 0
v_and_b32_e32 v12, -2, v0
v_add_i32_e32 v12, vcc, 0x1800, v12 ; the halfword of the two bytes 2k and 2k + 1
s_waitcnt lgkmcnt(0)
s_barrier
ds_read_u8 v8, v4
ds_read_i8 v9, v4
ds_read_u16 v10, v6
ds_read_i16 v11, v6
ds_read_u16 v14, v12
ds_read_b128 v[20:23], v13
ds_read_b64 v[24:25], v13 offset:8
ds_read_b32 v29, v30                ; dword 0 is still 0 after the dropped write
ds_read_u8 v37, v32
ds_read_u16 v38, v34
s_waitcnt lgkmcnt(0)
flat_store_dword v[2:3], v8
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v9
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v10
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v11
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v14
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v21
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v23
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v24
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v26
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v29
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v37
v_add_i32_e32 v2, vcc, 0x200, v2
flat_store_dword v[2:3], v38
s_endpgm

; Scalar memory. User data: s[0:1] the output address (0x2000-aligned), s[2:3] the guest address
; of a table of dwords, s[4:7] a V# over the same table. s_load_* reads through the pointer, and
; s_buffer_load_* through the V#, whose size the test changes. Every lane writes the 18 values to
; out[64 * k + i]. The loads run with EXEC = 0: scalar memory runs in every lane regardless.
; An offset in an SGPR is in bytes, and its low 2 bits are ignored.
s_mov_b64 exec, 0
s_load_dword s16, s[2:3], 0x1
s_load_dwordx2 s[18:19], s[2:3], 0x2
s_load_dwordx4 s[20:23], s[2:3], 0x4
s_mov_b32 s17, 36
s_load_dword s25, s[2:3], s17           ; the offset in an SGPR: byte 36, dword 9
s_mov_b32 s30, 19
s_buffer_load_dword s26, s[4:7], 0x1
s_buffer_load_dwordx2 s[28:29], s[4:7], 0x5
s_buffer_load_dwordx4 s[32:35], s[4:7], 0x4
s_buffer_load_dword s27, s[4:7], 0x40   ; far out of range
s_buffer_load_dword s36, s[4:7], s17    ; byte 36, dword 9: in range for the 4 KiB buffer
s_buffer_load_dword s37, s[4:7], s30    ; byte 19 reads dword 4
s_waitcnt lgkmcnt(0)
s_mov_b64 exec, -1
v_lshlrev_b32_e32 v1, 2, v0
v_or_b32_e32 v2, s0, v1                 ; v[2:3] = &out[i]
v_mov_b32_e32 v3, s1
v_mov_b32_e32 v6, s1
v_mov_b32_e32 v4, s16
flat_store_dword v[2:3], v4
v_mov_b32_e32 v4, s18
v_or_b32_e32 v5, 0x100, v2
flat_store_dword v[5:6], v4
v_mov_b32_e32 v4, s19
v_or_b32_e32 v5, 0x200, v2
flat_store_dword v[5:6], v4
v_mov_b32_e32 v4, s20
v_or_b32_e32 v5, 0x300, v2
flat_store_dword v[5:6], v4
v_mov_b32_e32 v4, s21
v_or_b32_e32 v5, 0x400, v2
flat_store_dword v[5:6], v4
v_mov_b32_e32 v4, s22
v_or_b32_e32 v5, 0x500, v2
flat_store_dword v[5:6], v4
v_mov_b32_e32 v4, s23
v_or_b32_e32 v5, 0x600, v2
flat_store_dword v[5:6], v4
v_mov_b32_e32 v4, s25
v_or_b32_e32 v5, 0x700, v2
flat_store_dword v[5:6], v4
v_mov_b32_e32 v4, s26
v_or_b32_e32 v5, 0x800, v2
flat_store_dword v[5:6], v4
v_mov_b32_e32 v4, s28
v_or_b32_e32 v5, 0x900, v2
flat_store_dword v[5:6], v4
v_mov_b32_e32 v4, s29
v_or_b32_e32 v5, 0xa00, v2
flat_store_dword v[5:6], v4
v_mov_b32_e32 v4, s32
v_or_b32_e32 v5, 0xb00, v2
flat_store_dword v[5:6], v4
v_mov_b32_e32 v4, s33
v_or_b32_e32 v5, 0xc00, v2
flat_store_dword v[5:6], v4
v_mov_b32_e32 v4, s34
v_or_b32_e32 v5, 0xd00, v2
flat_store_dword v[5:6], v4
v_mov_b32_e32 v4, s35
v_or_b32_e32 v5, 0xe00, v2
flat_store_dword v[5:6], v4
v_mov_b32_e32 v4, s27
v_or_b32_e32 v5, 0xf00, v2
flat_store_dword v[5:6], v4
v_mov_b32_e32 v4, s36
v_or_b32_e32 v5, 0x1000, v2
flat_store_dword v[5:6], v4
v_mov_b32_e32 v4, s37
v_or_b32_e32 v5, 0x1100, v2
flat_store_dword v[5:6], v4
s_endpgm

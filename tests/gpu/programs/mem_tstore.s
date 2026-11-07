; Typed buffer stores. User data: s[4:7] the V# of the input (lane i's 16 bytes at 16 * i: floats
; in the first KB, integers in the second), s[8:11] the V# of the output. Block k (1 KB at
; 1 KB * k) gets one format, the element of lane i at byte i (v0), 2 * i (v2) or 16 * i (v1),
; moved by the offset modifier: adjacent lanes share a dword in the narrow formats, and the 8_8
; elements at 2 * i + 1 cross a dword boundary in every other lane.
v_lshlrev_b32_e32 v1, 4, v0
v_lshlrev_b32_e32 v2, 1, v0
buffer_load_dwordx4 v[16:19], v1, s[4:7], 0 offen
buffer_load_dwordx4 v[20:23], v1, s[4:7], 0 offen offset:1024
s_waitcnt vmcnt(0)
s_mov_b32 s20, 0x0
tbuffer_store_format_x v16, v0, s[8:11], s20 format:[BUF_DATA_FORMAT_8,BUF_NUM_FORMAT_UNORM] offen
s_mov_b32 s20, 0x400
tbuffer_store_format_x v20, v0, s[8:11], s20 format:[BUF_DATA_FORMAT_8,BUF_NUM_FORMAT_SINT] offen
s_mov_b32 s20, 0x800
tbuffer_store_format_x v16, v2, s[8:11], s20 format:[BUF_DATA_FORMAT_16,BUF_NUM_FORMAT_FLOAT] offen
s_mov_b32 s20, 0xc00
tbuffer_store_format_x v20, v2, s[8:11], s20 format:[BUF_DATA_FORMAT_16,BUF_NUM_FORMAT_UINT] offen
s_mov_b32 s20, 0x1000
tbuffer_store_format_xy v[16:17], v2, s[8:11], s20 format:[BUF_DATA_FORMAT_8_8,BUF_NUM_FORMAT_SNORM] offen offset:1
s_mov_b32 s20, 0x1400
tbuffer_store_format_xy v[16:17], v1, s[8:11], s20 format:[BUF_DATA_FORMAT_16_16,BUF_NUM_FORMAT_UNORM] offen
s_mov_b32 s20, 0x1800
tbuffer_store_format_xy v[20:21], v1, s[8:11], s20 format:[BUF_DATA_FORMAT_16_16,BUF_NUM_FORMAT_SINT] offen
s_mov_b32 s20, 0x1c00
tbuffer_store_format_xy v[16:17], v1, s[8:11], s20 format:[BUF_DATA_FORMAT_8_8_8_8,BUF_NUM_FORMAT_UNORM] offen
s_mov_b32 s20, 0x2000
tbuffer_store_format_xyzw v[16:19], v1, s[8:11], s20 format:[BUF_DATA_FORMAT_2_10_10_10,BUF_NUM_FORMAT_SNORM] offen
s_mov_b32 s20, 0x2400
tbuffer_store_format_xyzw v[20:23], v1, s[8:11], s20 format:[BUF_DATA_FORMAT_2_10_10_10,BUF_NUM_FORMAT_UINT] offen
s_mov_b32 s20, 0x2800
tbuffer_store_format_xyzw v[16:19], v1, s[8:11], s20 format:[BUF_DATA_FORMAT_16_16_16_16,BUF_NUM_FORMAT_FLOAT] offen
s_mov_b32 s20, 0x2c00
tbuffer_store_format_xyz v[16:18], v1, s[8:11], s20 format:[BUF_DATA_FORMAT_32_32_32,BUF_NUM_FORMAT_FLOAT] offen offset:1
s_mov_b32 s20, 0x3000
tbuffer_store_format_xyzw v[20:23], v1, s[8:11], s20 format:[BUF_DATA_FORMAT_8_8_8_8,BUF_NUM_FORMAT_SINT] offen offset:3
s_mov_b32 s20, 0x3400
tbuffer_store_format_xyz v[16:18], v1, s[8:11], s20 format:[BUF_DATA_FORMAT_10_11_11,BUF_NUM_FORMAT_FLOAT] offen
s_endpgm

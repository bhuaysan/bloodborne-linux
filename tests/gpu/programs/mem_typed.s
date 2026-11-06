; Typed buffer loads. User data: s[4:7] the V# of a table of 2 KB, s[8:11] the V# of the output
; buffer. Lane i loads the 16 bytes at 16 * i of the table, as one format per block (the offset
; modifiers move the element, three blocks put a wide element at an odd byte), and
; stores the four components to out[1 KB * block + 16 * i].
v_lshlrev_b32_e32 v1, 4, v0
s_mov_b32 s20, 0x0
tbuffer_load_format_xyzw v[16:19], v1, s[4:7], 0 format:[BUF_DATA_FORMAT_8_8_8_8,BUF_NUM_FORMAT_UNORM] offen
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[16:19], v1, s[8:11], s20 offen
s_mov_b32 s20, 0x400
tbuffer_load_format_xyzw v[16:19], v1, s[4:7], 0 format:[BUF_DATA_FORMAT_8_8_8_8,BUF_NUM_FORMAT_SSCALED] offen
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[16:19], v1, s[8:11], s20 offen
s_mov_b32 s20, 0x800
tbuffer_load_format_xyzw v[16:19], v1, s[4:7], 0 format:[BUF_DATA_FORMAT_8,BUF_NUM_FORMAT_SINT] offen offset:1
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[16:19], v1, s[8:11], s20 offen
s_mov_b32 s20, 0xc00
tbuffer_load_format_xyzw v[16:19], v1, s[4:7], 0 format:[BUF_DATA_FORMAT_8_8,BUF_NUM_FORMAT_SNORM] offen offset:3
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[16:19], v1, s[8:11], s20 offen
s_mov_b32 s20, 0x1000
tbuffer_load_format_xyzw v[16:19], v1, s[4:7], 0 format:[BUF_DATA_FORMAT_16_16,BUF_NUM_FORMAT_FLOAT] offen offset:1024
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[16:19], v1, s[8:11], s20 offen
s_mov_b32 s20, 0x1400
tbuffer_load_format_xyzw v[16:19], v1, s[4:7], 0 format:[BUF_DATA_FORMAT_16_16_16_16,BUF_NUM_FORMAT_SNORM] offen
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[16:19], v1, s[8:11], s20 offen
s_mov_b32 s20, 0x1800
tbuffer_load_format_xyzw v[16:19], v1, s[4:7], 0 format:[BUF_DATA_FORMAT_16,BUF_NUM_FORMAT_UINT] offen offset:2
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[16:19], v1, s[8:11], s20 offen
s_mov_b32 s20, 0x1c00
tbuffer_load_format_xyzw v[16:19], v1, s[4:7], 0 format:[BUF_DATA_FORMAT_16_16,BUF_NUM_FORMAT_SNORM_OGL] offen
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[16:19], v1, s[8:11], s20 offen
s_mov_b32 s20, 0x2000
tbuffer_load_format_xyzw v[16:19], v1, s[4:7], 0 format:[BUF_DATA_FORMAT_32_32_32,BUF_NUM_FORMAT_FLOAT] offen
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[16:19], v1, s[8:11], s20 offen
s_mov_b32 s20, 0x2400
tbuffer_load_format_xyzw v[16:19], v1, s[4:7], 0 format:[BUF_DATA_FORMAT_32_32_32_32,BUF_NUM_FORMAT_UINT] offen
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[16:19], v1, s[8:11], s20 offen
s_mov_b32 s20, 0x2800
tbuffer_load_format_xyzw v[16:19], v1, s[4:7], 0 format:[BUF_DATA_FORMAT_2_10_10_10,BUF_NUM_FORMAT_UNORM] offen
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[16:19], v1, s[8:11], s20 offen
s_mov_b32 s20, 0x2c00
tbuffer_load_format_xyzw v[16:19], v1, s[4:7], 0 format:[BUF_DATA_FORMAT_2_10_10_10,BUF_NUM_FORMAT_SINT] offen
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[16:19], v1, s[8:11], s20 offen
s_mov_b32 s20, 0x3000
tbuffer_load_format_xyzw v[16:19], v1, s[4:7], 0 format:[BUF_DATA_FORMAT_16_16,BUF_NUM_FORMAT_UNORM] offen offset:2
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[16:19], v1, s[8:11], s20 offen
s_mov_b32 s20, 0x3400
tbuffer_load_format_xyzw v[16:19], v1, s[4:7], 0 format:[BUF_DATA_FORMAT_8_8_8_8,BUF_NUM_FORMAT_UINT] offen offset:3
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[16:19], v1, s[8:11], s20 offen
s_mov_b32 s20, 0x3800
tbuffer_load_format_xyzw v[16:19], v1, s[4:7], 0 format:[BUF_DATA_FORMAT_32_32,BUF_NUM_FORMAT_SINT] offen offset:1
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[16:19], v1, s[8:11], s20 offen
s_mov_b32 s20, 0x3c00
tbuffer_load_format_xyzw v[16:19], v1, s[4:7], 0 format:[BUF_DATA_FORMAT_16_16,BUF_NUM_FORMAT_USCALED] offen offset:4
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[16:19], v1, s[8:11], s20 offen
s_endpgm

; Formatted MUBUF and tfe. User data: s[4:7] the V# of a 4 KB buffer with the format and dst_sel
; under test, s[8:11] the V# of the output, s12 the log2 of the stores' lane stride. Lane i loads
; the element at 16 * i in the V#'s format with tfe (its fifth VGPR preset to -1) and writes the
; four components to out[16 * i] and the tfe VGPR to out[0x400 + 4 * i]; loads the dword at
; 16 * i as MTBUF 32 UINT into out[0x600 + 8 * i] with v22 (the test sets the tfe bit, which
; llvm-mc rejects for MTBUF); and stores the four dwords at 0x800 + 16 * i in the V#'s format
; at 0xc00 + (i << s12).
v_lshlrev_b32_e32 v1, 4, v0
v_mov_b32_e32 v20, -1
buffer_load_format_xyzw v[16:20], v1, s[4:7], 0 offen tfe
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[16:19], v1, s[8:11], 0 offen
v_lshlrev_b32_e32 v2, 2, v0
s_movk_i32 s20, 0x400
buffer_store_dword v20, v2, s[8:11], s20 offen
v_mov_b32_e32 v22, -1
tbuffer_load_format_x v21, v1, s[4:7], 0 format:[BUF_DATA_FORMAT_32,BUF_NUM_FORMAT_UINT] offen
v_lshlrev_b32_e32 v2, 3, v0
s_movk_i32 s20, 0x600
s_waitcnt vmcnt(0)
buffer_store_dwordx2 v[21:22], v2, s[8:11], s20 offen
s_movk_i32 s20, 0x800
buffer_load_dwordx4 v[24:27], v1, s[4:7], s20 offen
v_lshlrev_b32_e32 v3, s12, v0
s_movk_i32 s20, 0xc00
s_waitcnt vmcnt(0)
buffer_store_format_xyzw v[24:27], v3, s[4:7], s20 offen
s_endpgm

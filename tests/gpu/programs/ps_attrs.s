; A pixel shader (key in stages_test.cpp). User data: s0 and s1; the primitive mask follows in s2.
; Inputs: v[0:1] the perspective barycentrics at the center, v[2:3] the linear ones at the
; centroid, v4 and v5 the X and Y of the position. It keeps the pixels whose X is at least 0.5,
; and exports mrt0 (32_ABGR), mrt2 (32_GR) and mrt1 (FP16, 0.5 and 1.0).
s_mov_b32 m0, s2
v_cmpx_le_u32_e32 vcc, 0.5, v4
v_mov_b32_e32 v10, 0x3c003800
exp mrt0 v4, v5, v4, v5
exp mrt2 v4, v5, off, off
exp mrt1 v10, v10, v10, v10 done compr vm
s_endpgm

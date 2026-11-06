; A pixel shader (key in stages_test.cpp). User data: s0 and s1; the primitive mask follows in s2.
; Inputs: v[0:1] the perspective barycentrics at the center, v[2:3] the linear ones at the
; centroid, v4 and v5 the X and Y of the position. It interpolates attr0 (perspective), attr1
; (linear, centroid), attr2 (flat) and attr3 (a default value), keeps the pixels whose attr0.x is
; at least 0.5, and exports mrt0 (32_ABGR), mrt2 (32_GR), the Y position as the depth and mrt1
; (FP16, 0.5 and 1.0).
s_mov_b32 m0, s2
v_interp_p1_f32 v6, v0, attr0.x
v_interp_p2_f32 v6, v1, attr0.x
v_interp_p1_f32 v7, v0, attr0.y
v_interp_p2_f32 v7, v1, attr0.y
v_interp_p1_f32 v8, v2, attr1.z
v_interp_p2_f32 v8, v3, attr1.z
v_interp_mov_f32 v9, p0, attr2.w
v_interp_p1_f32 v12, v0, attr3.x
v_interp_p2_f32 v12, v1, attr3.x
v_cmpx_le_u32_e32 vcc, 0.5, v6
v_mov_b32_e32 v10, 0x3c003800
exp mrt0 v6, v7, v8, v12
exp mrt2 v9, v5, off, off
exp mrtz v5, off, off, off
exp mrt1 v10, v10, v10, v10 done compr vm
s_endpgm

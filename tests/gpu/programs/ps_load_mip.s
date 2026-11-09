; A pixel shader for the image load test (tests/gpu/runner/image_render_test.cpp), key in the test:
; s[0:7] a T#, the primitive mask in s12, v[0:1] the barycentrics at the center. It interpolates
; attr0 (texel x, y, then z and w below) and converts it to integers into v[6:9]. image_load_mip
; then reads red and green at level v8 of layer 0, and with da blue and alpha at slice v8 and
; level v9; for a 2D image the slice is ignored. It exports the four to mrt0 (32_ABGR).
s_mov_b32 m0, s12
v_interp_p1_f32 v6, v0, attr0.x
v_interp_p2_f32 v6, v1, attr0.x
v_interp_p1_f32 v7, v0, attr0.y
v_interp_p2_f32 v7, v1, attr0.y
v_interp_p1_f32 v8, v0, attr0.z
v_interp_p2_f32 v8, v1, attr0.z
v_interp_p1_f32 v9, v0, attr0.w
v_interp_p2_f32 v9, v1, attr0.w
v_cvt_u32_f32_e32 v6, v6
v_cvt_u32_f32_e32 v7, v7
v_cvt_u32_f32_e32 v8, v8
v_cvt_u32_f32_e32 v9, v9
image_load_mip v[10:11], v6, s[0:7] dmask:0x3 unorm
image_load_mip v[12:13], v6, s[0:7] dmask:0xc unorm da
exp mrt0 v10, v11, v12, v13 done vm
s_endpgm

; A compute program for the image store test (tests/gpu/runner/image_render_test.cpp): s[0:7]
; the T#, v0 the thread index in x (8 threads in x). Lane 3 is inactive for the stores. Each lane
; stores to two places: (x, 1) with dmask 0x9 (red x as a float, alpha 0.5), and (x, 2) of slice 1
; with da and dmask 0x7 (x as a float, 2.0, 4.0).
v_cvt_f32_u32_e32 v8, v0
v_mov_b32_e32 v6, v8
v_mov_b32_e32 v7, 0.5
v_mov_b32_e32 v9, 2.0
v_mov_b32_e32 v10, 4.0
v_mov_b32_e32 v1, 1
v_mov_b32_e32 v3, v0
v_mov_b32_e32 v4, 2
v_mov_b32_e32 v5, 1
v_cmpx_ne_u32_e32 vcc, 3, v0
image_store v[6:7], v0, s[0:7] dmask:0x9 unorm
image_store v[8:10], v3, s[0:7] dmask:0x7 unorm da
s_endpgm

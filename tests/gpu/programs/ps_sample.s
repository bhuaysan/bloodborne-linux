; A pixel shader for the image render test (tests/gpu/runner/image_render_test.cpp). User data:
; s[0:7] a T#, s[8:11] an S#; the primitive mask is in s12. Inputs: v[0:1] the perspective
; barycentrics at the center. In whole quad mode, as the implicit LOD needs the helper lanes'
; coordinates, it interpolates attr0 into v[6:9] and samples the image four ways, one component
; each: x with implicit LOD, y at the LOD in v9, z with the derivatives (0.5, 0, 0, 0.5) in
; v[2:5], w at LOD 0. The y and z samples have da, so v8 is their slice (or, for a cube, every
; sample's face_id). With EXEC back to the covered pixels, it exports the four to mrt0 (32_ABGR).
s_mov_b64 s[14:15], exec
s_wqm_b64 exec, exec
s_mov_b32 m0, s12
v_interp_p1_f32 v6, v0, attr0.x
v_interp_p2_f32 v6, v1, attr0.x
v_interp_p1_f32 v7, v0, attr0.y
v_interp_p2_f32 v7, v1, attr0.y
v_interp_p1_f32 v8, v0, attr0.z
v_interp_p2_f32 v8, v1, attr0.z
v_interp_p1_f32 v9, v0, attr0.w
v_interp_p2_f32 v9, v1, attr0.w
v_mov_b32_e32 v2, 0.5
v_mov_b32_e32 v3, 0
v_mov_b32_e32 v4, 0
v_mov_b32_e32 v5, 0.5
image_sample v10, v6, s[0:7], s[8:11] dmask:0x1
image_sample_l v11, v6, s[0:7], s[8:11] dmask:0x2 da
image_sample_d v12, v[2:3], s[0:7], s[8:11] dmask:0x4 da
image_sample_lz v13, v6, s[0:7], s[8:11] dmask:0x8
s_waitcnt vmcnt(0)
s_and_b64 exec, exec, s[14:15]
exp mrt0 v10, v11, v12, v13 done vm
s_endpgm

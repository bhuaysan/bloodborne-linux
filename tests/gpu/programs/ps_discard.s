; A pixel shader for the render test (tests/gpu/runner/render_test.cpp). No user data: the
; primitive mask is in s0. Inputs: v[0:1] the perspective barycentrics at the center. In whole
; quad mode, it interpolates attr0; then, with EXEC back to the covered pixels, it keeps those
; whose attr0.x is at least 0.5 and exports attr0 to mrt0 (32_ABGR).
s_mov_b64 s[2:3], exec
s_wqm_b64 exec, exec
s_mov_b32 m0, s0
v_interp_p1_f32 v2, v0, attr0.x
v_interp_p2_f32 v2, v1, attr0.x
v_interp_p1_f32 v3, v0, attr0.y
v_interp_p2_f32 v3, v1, attr0.y
v_interp_p1_f32 v4, v0, attr0.z
v_interp_p2_f32 v4, v1, attr0.z
v_interp_p1_f32 v5, v0, attr0.w
v_interp_p2_f32 v5, v1, attr0.w
s_and_b64 exec, exec, s[2:3]
v_cmpx_le_f32_e32 vcc, 0.5, v2
exp mrt0 v2, v3, v4, v5 done vm
s_endpgm

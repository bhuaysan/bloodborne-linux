; A vertex shader (key in stages_test.cpp). User data: s[0:3] the V# of the vertices, 32 bytes
; each: a position, then a color. It loads both by the vertex index in v0 and exports them as pos0
; and param0, and the instance index (v3, with VGPR_COMP_CNT 3) as the x of param1.
buffer_load_dwordx4 v[4:7], v0, s[0:3], 0 idxen
buffer_load_dwordx4 v[8:11], v0, s[0:3], 0 idxen offset:16
s_waitcnt vmcnt(0)
exp pos0 v4, v5, v6, v7 done
exp param0 v8, v9, v10, v11
exp param1 v3, off, off, off
s_endpgm

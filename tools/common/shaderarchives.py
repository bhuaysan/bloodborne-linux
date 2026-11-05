"""Pinned SHA-256 digests of the shader archives in dvdroot_ps4/shader/ (CUSA03173 patch 1.09).

tools/shaderbnd reads only these files. The design table in docs/design/graphics.md lists the
same archives with their file and program counts.
"""

SHADER_DIR = "dvdroot_ps4/shader"

SHADER_ARCHIVES = {
    "b07d5bcb1853ad541874f90527c699a3427cfb8f359c0544dbad76628380ef06": "gxdecal.shaderbnd.dcx",
    "c1a0e7bbdc244624bf0c945bb483cf577247eb890c3af45ec730d54630e3340a": "gxffxshader.shaderbnd.dcx",
    "40edc29b23cb821d8d3e45a8e90755286fc4789d09d2de601b0396c1b86fc4d8": "gxflvershader.shaderbnd.dcx",
    "5c50dd7502525064ce3c10da22d76426921d3b338e28c08524140ecdee7db2f3":
        "gxflvershader.shaderbnddebug.dcx",
    "6eed07f097bc0b6a53fbea1bfb9fee65559026fbb97508adb7d3608b6eebf2b3": "gxgui.shaderbnd.dcx",
    "64f688aa99d97bb8261cff6e42c33692c1fe612e5d09f072c4c1e82fe8e6b03a": "gxposteffect.shaderbnd.dcx",
    "2d5f6d714c23e1e1411faa42c37a8f7e796c9dfb3947118508c214047411c9d6":
        "gxrenderershader.shaderbnd.dcx",
    "5ac010ca3f2cfa49bc1a27f74d3c8901999e9a366c1438cff394ac4ddda1c4a5": "gxshader.shaderbnd.dcx",
}

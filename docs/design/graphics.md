# Graphics design (M4, removed)

This note described our own GPU path of M4: a GCN translator, a PM4 parser, a Vulkan backend and a GPU memory mirror under `src/gpu`. #435 removed that code, and the note's content went with it. Its history is in git.

The two sections that code still cites moved:

- "Shader extraction", the table of shader archives with their file and program counts: [formats/shader-binary.md](../formats/shader-binary.md#shader-archives).
- "Stage register imports", the register blocks of `sceGnmSet*Shader`: [formats/gnm.md](../formats/gnm.md#stage-register-imports).

The renderer and the shader translator now come from shadPS4: see [renderer.md](renderer.md) and [rewrite.md](rewrite.md).

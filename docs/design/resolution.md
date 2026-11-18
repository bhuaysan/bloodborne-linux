# Resolution (R8.4)

How the game sizes its render targets, which way #515 renders above 1080p, and how that work is split (#511). This is our own analysis of the pinned eboot; third-party resolution patches are off-limits (`docs/source-policy.md`). Addresses are eboot link addresses (the guest runs them at 0x10000000 + address).

## Decision: way B, the game's own frame buffer size

`bb-run` sets the engine's frame buffer size before the game starts (way B of #511). It does not scale the game's targets in the renderer (way A).

The engine sizes everything from one pair of globals, `frame_buffer_width` at 0x51289f8 and `frame_buffer_height` at 0x51289fc. Their data defaults are 1920 and 1080. On the PS4 path only `system_properties_init` writes them, with the values it just read from them (below), and the probe read 1920×1080 there in every frame of a 1x run. With other values written there before the game runs, the engine sizes every screen-sized target from them. That covers the colour and depth targets, the half- and quarter-size targets, the YEBIS post-processing chain, the compute grids, the viewports and scissors, and the display buffers it registers with video out. The 2560×1440 prototype run matched the 1x frame pass for pass and draw for draw (see "The prototype runs").

Way A would rebuild all of that in the renderer:

- **Fragment coordinates and fetches.** The final pass reads the tone-mapped image with an image fetch at `FragCoord`, and the lighting passes fetch from the G-buffer and depth (the probe's `fetch?` flag below). Scaled images would need scaled fetch coordinates in the shader translator, per image, and unscaled ones for the images that keep their size.
- **Compute grids and buffer clears.** The game computes them on the CPU from 1920×1080. One compute pass over the HDR image runs 240×135 groups of 8×8, and a shader without images (`3d5ebf4e`, most likely a buffer fill) runs a grid proportional to the pixel count. The renderer would have to scale the grid and remap every coordinate in the shader.
- **Which targets are screen-derived.** The YEBIS chain uses sizes such as 1176×441, 628×530 and 314×265 that are not simple fractions of 1080p. The renderer could only guess which targets to scale; the engine knows.
- **Guest memory.** A scaled host image no longer matches the guest memory the game allocated for it. The texture cache is keyed by guest address and size, so a scaled image needs a backing of its own, and every path that sees the target as guest memory would have to know about it.

Way B gets all of this right without a change to the renderer, the shader translator or the texture cache. Its cost is memory: the engine allocates the larger targets from its own fixed pools, and at 3840×2160 they no longer leave room for the map (below). Way A would keep the guest's targets at 1080p and put the scaled copies in host memory, so it would not hit that limit. Growing a pool of the engine is one targeted change (#640); the work of way A is spread over the translator, the texture cache and the command processor, and stays a guess for every target the renderer can't identify.

## Where the size comes from

- **The globals.** `frame_buffer_width` (0x51289f8) and `frame_buffer_height` (0x51289fc) are initialized data, 0x780 and 0x438. About 20 functions read them.
- **System properties.** `system_properties_init` (0x2017770, `SprjSystemProperties`) reads the configuration keys `SPRJ.WIN64.frameBufferW` and `SPRJ.WIN64.frameBufferH` with the globals as fallbacks (`config_get_int`, 0x20eba00) and writes the results back. It also stores the scale factors width/1920 and height/1080 at 0x55404d0 and 0x55404d4, which the HUD and menu layout read (0x1816a50, 0x1817690, 0x18179c0, 0x181a9c0) to place their elements. The keys are the PC version's settings, and the PS4 game has no configuration that sets them, so the defaults hold.
- **A setter.** `set_frame_buffer_size` (0x1d9e850) writes both globals and both scale factors from its arguments (`esi`, `edx`). It is slot +0x18 of the vtable at 0x534dca0, most likely a window-resize callback of the PC version. It has no direct call site.
- **The graphics object.** `graphics_init` (0x1d969f0) builds the graphics device parameters from the globals. `graphics_create` (0x2194580) copies them into the graphics object (pointer at 0x55406c8, the object of `docs/design/frame-rate.md`, "The present"), rounds width and height up to multiples of 4 (+0xf0, +0xf4) and gives every swap chain that size when the parameters name none. The swap chains' buffers are the display buffers the game registers with `sceVideoOutSetBufferAttribute` and `sceVideoOutRegisterBuffers` on its main video out path (0x26d5e90, width and height from the swap chain description). The camera viewport descriptions take their size from an element of a list in the graphics object, `[[0x55406c8]+0x90]+0x80` and `+0x84` (0x1da7aa0); the probe read 1920×1080 there at 1x and the frame buffer size in every prototype run.
- **Other readers.** The debug screenshot capture (0x1db5a40, full, half and quarter size), the HUD (0x1654d80) and the debug memory overlay (0x201ae30) read the globals directly.

Other 1920 and 1080 constants in the code are not render target sizes: the HUD keeps elements inside 1920.0×1080.0 (0x1bfb2a0), a dead PC display descriptor (0x26dbdb0, `\\.\DISPLAY1`, no references) and the boot video out path (0x11e4620, two 1920×1080 buffers for the logos). The logos were not checked at another size; #515 checks them.

`bb-run` writes the two globals once, after the eboot is mapped and before its entry point runs. `system_properties_init` then reads them as its fallbacks and computes the scale factors from them, and the graphics object is created from them later.

## The render targets of a frame

A local probe (not committed) logged every pass of every 150th frame: the colour and depth targets with their guest address, size and format, the viewports and scissors, the pixel shaders, the images read, and every compute dispatch with its grid and images. The flag `fetch?` marks a pass with at least one image that has no sampler, which is an image fetch at integer coordinates. A pass is a run of draws with the same targets.

**Title menu (4 passes, 3 dispatches).** The depth target is cleared (D32S8, 1920×1080). The menu is drawn into an RGBA8 1920×1080 target. A compute shader updates a 1025×1 R32F image. The final pass fetches the menu image at `FragCoord` and writes the display buffer (RGBA8 sRGB, 1920×1080).

**Clinic (one frame, 221 passes, 93 dispatches).** Grouped by size:

| Targets | Size at 1x | Passes (draws per pass) | Read by |
|---|---|---|---|
| Depth and stencil D32S8 alone | 1920×1080 | 10 (1–2) | |
| G-buffer: 4 RGBA8 (one sRGB view), B10G11R11F and RGBA16F, with depth; once with a 7th RGBA16F | 1920×1080 | 1 (505), 1 (3) | the lighting passes, by fetch |
| Lighting: 2 B10G11R11F, with and without depth | 1920×1080 | 1 (348), 1 (1) | the HDR passes |
| RGBA16F (the HDR scene), alone or with depth; once two | 1920×1080 | 14 (1–86) | YEBIS; a compute pass of 240×135 groups of 8×8 on it |
| RGBA8, alone or with depth (the tone-mapped output with the HUD on top, and helpers) | 1920×1080 | 22 (1–2) | the final pass, by fetch at `FragCoord` |
| No target, full-screen and clip-disabled | 1920×1080 | 8 (1–2) | |
| Half size: RGBA8, RGBA16F, R32F, and D32S8 alone and with RGBA8 | 960×540 | 15 (1–36), 2 without a target | sampled by full-size passes |
| Quarter size: D32S8, RGBA16F | 480×270 | 2, 1 without a target | |
| YEBIS post-processing: RGBA16F and B10G11R11F | 1176×441, 628×530, 314×265 down to 5×5 | 33 (1) | the next level and the composite |
| Display buffer RGBA8 sRGB | 1920×1080 | the final pass (1) | video out |
| Not screen-sized: shadow map D32, four viewports of 2048×2048 | 4096×4096 | 1 (44), 1 without a target | |
| Not screen-sized: R16G16F with D32 | 256², 128² (18 passes each), 64², 32² (36 each) | 108 (1–34) | compute passes of 32×32 groups |

Every viewport and scissor covers its whole target in pixels, set by the game per pass. Clip-disabled passes (`NOCLIP`) are the full-screen passes; they also cover the whole target. The 1025×1 and 2048×2 compute images (the luminance) and the 64×64 compute images are not screen-sized.

## The prototype runs

The prototype is the probe build with the two globals written from an environment variable before the game starts. It ran the same scenario three times on the reference machine (2026-11-18): the title menu with a shot, Continue into the clinic from an `r5-acceptance` save, two clinic shots, a camera turn and a few steps, each with a shot. The window was 1920×1006 (3840×2160 requested, clamped by the window manager), so every shot is the presenter's scaled copy of the display buffer, taken by the rig's Vulkan layer.

| Frame buffer | Clinic frame | G-buffer pass | Shots | Peak memory | Frame time p50 / p99 |
|---|---|---|---|---|---|
| 1920×1080 | 221 passes, 93 dispatches | 505 draws, textures up to 4096×2048 | complete | 2891 MiB | 33.3 / 35.4 ms |
| 2560×1440 | 221 passes, 93 dispatches | 505 draws, the same shaders and textures | complete, the same scene | 2914 MiB | 33.3 / 36.0 ms |
| 3840×2160 | 188 passes, 87 dispatches | 280 draws, textures 128×128 and 256×256 | walls and floor missing, sky colour behind the shelves | 1867 MiB | 33.3 / 35.8 ms |

All three runs ended `scenario finished` with no GPU fault in the kernel log.

**What scales correctly, at both sizes:** every screen-sized target of the table (1920×1080 → 2560×1440 → 3840×2160, the same for the half and quarter sizes), the YEBIS chain (1176×441 → 1568×588 → 2352×882, 314×265 → 419×354 → 628×530), the viewports and scissors, the compute grids (240×135 → 320×180 → 480×270 groups, and the `3d5ebf4e` grids grow with the pixel count), the display buffer, the HUD layout and the title menu. The fixed-size targets keep their size.

**What fails, at 3840×2160 only:** the map's static geometry and its full-size textures are missing. The G-buffer pass has 280 draws instead of 505, without the shaders of the walls and floor, and samples only 128×128 and 256×256 textures. The game's memory peaked 1 GiB lower than at 1x. The game's guest memory ends with the same mappings in all three runs (21 regions, 314312 pages), so the 4× larger targets come out of the engine's own fixed pools. The map's resources and texture streaming then no longer fit, and the game drops them without a crash. 2560×1440 still fits.

## The graphics heap (#640)

The engine sets its heaps up at start from a size table in the eboot (0x4736d40): one row of 16 sizes for each of 9 build configurations, the heaps named at 0x5358cc0 (MAIN, GFX, GFX_SystemShared, GFX_GraphicsPrivate, GFX_GraphicsPrivateB, …). This build reads the row `Master_ps4_GFXMIX`, after the row `Master_ps4_flexible`, which only sizes CORERES, MENU and DEBUG. The render targets and the map's textures come from `GFX_GraphicsPrivateMix` (0x569d448), which joins `GFX_GraphicsPrivate` (2287 MiB) and `GFX_GraphicsPrivateB` (886 MiB): blocks of 1 MiB and more go to the first, smaller ones to the second, and each spills into the other (0x1fbeb30).

That is the pool that runs out. A local probe read both heaps' free bytes once a second through the clinic scenario of #511:

| Frame buffer | `GFX_GraphicsPrivate` | Least free in both | Taken beyond 1x |
|---|---|---|---|
| 1920×1080 | 2287 MiB | 600 MiB | |
| 2560×1440 | 2287 MiB | 194 MiB | 406 MiB (264 bytes per pixel above 1080p) |
| 3840×2160 | 2287 MiB | 3 MiB, the map dropped | |
| 3840×2160 | 3901 MiB | 572 MiB | 1642 MiB (277 bytes per pixel) |

The need grows with the pixels: the larger targets, and nothing else. `bb-run` writes the size of `GFX_GraphicsPrivate` in that table before the game starts, 2287 MiB plus 288 bytes for each pixel above 1920×1080 (the larger measured rate plus 4 %), so every size keeps at least the free space of 1x: 2730 MiB at 2560×1440, 3996 MiB at 3840×2160. Growing `GFX_GraphicsPrivateB` instead would not help the large blocks, which only spill into it. The heap's backing is direct memory, of which the kernel layer has 8 GiB; with the larger heap the game's heaps take about 6 GiB of it at 3840×2160. At 1920×1080 the table keeps its own value.

## Work for #515

1. **#515: the option.** `bb-run --resolution WxH` (default 1920x1080) writes the two globals before the game starts, as the prototype did, in `src/game/gfx/`, with a unit test of the option parsing and the write. Sizes are 16:9 and at most 2560×1440 until #640 raises the limit; other aspect ratios need their own check of the HUD layout, which scales width and height separately. `rig.py compare` scales a shot to the other's size before comparing, so a run at another size can be compared with a 1x run. The option takes widths that are multiples of 64, so the half- and quarter-size targets have whole sides as they do at 1080p, and nothing below 1920×1080, which no run has checked. The launcher's picture settings (`docs/design/launcher.md`) get the option in an issue of R9 once it lands. Check: the clinic and `rig.py perf clinic-fight` at 2560×1440 without a crash, GPU fault or validation error, the frames matching 1x frames scaled down, the logos and the title menu right, 1x unchanged against `main`, frame times at both sizes in the issue. About 200 changed lines.
2. **#640: 3840×2160.** Find which engine pool runs out at 3840×2160 and grow it so the map loads as it does at 1x, then raise the option's limit to 3840×2160 (done: "The graphics heap (#640)"). Check: at 3840×2160 the clinic frame has the passes and draws of the 1x frame, the shots match 1x, and `rig.py perf clinic-fight` and `yharnam-route` run with frame times in the issue. Depends on #515.

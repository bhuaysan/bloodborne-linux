# Image quality (R11)

Where the engine switches its post-processing effects on and sets their strength (#659), for the switches of #665. Our own analysis of the pinned eboot; third-party patches and mods are off-limits (`docs/source-policy.md`). Addresses are eboot link addresses (the guest runs them at 0x10000000 + address). The field names come from the game's own debug menu (below), the YEBIS function names from the library's assertion and warning strings in the eboot.

## Result

| Effect | In the clinic and Central Yharnam | Switch | Clean |
|---|---|---|---|
| Motion blur | on, velocity based | block field `0x77` = 0 | yes |
| Chromatic aberration | on, strongest at the screen edges | block field `0x2b` = 0 | yes |
| Depth of field | on, but only beyond 100 m | block field `0x6b` = 0 | yes; no visible change on either route, 3 passes fewer |
| Bloom (glare) | on | block field `0x37` (glare luminance) = 0 | yes; YEBIS then skips the glare chain, 33 passes |
| Vignette | off (power 0) | block field `0x34` | nothing to switch |
| Film grain, lens distortion | never enabled | none | nothing to switch |

All of them are fields of one parameter block that the game rebuilds every frame from the map's draw parameters and hands to YEBIS on the render thread. The clean place for a switch is the call that applies the block (`pfx_apply`, 0x021d5f80): overwrite the field in the block it receives, every frame, before it runs. A single write before the game starts does not hold, and neither does one write to the global copy during the game: the game writes the global from the map's parameters again in the next frame.

## How the block flows

1. **The map's draw parameters.** Each map has a graphics parameter file, `param/drawparam/<map>.gparam.dcx` (cutscenes and special scenes have `s*` files of their own). The engine loads its groups into banks of the scene renderer (`+0x5970` to `+0x59b0` of the object 0x0229e990 gets as its first argument). Each group is looked up by an id pair, and the scene keeps two ids per group, the current and the target one, and a blend weight per group (`+0x2a78`, `+0x2ae0` and `+0x2b48` of its draw context), and blends the two by the weight (most likely the ease between areas with different parameters; not checked).
2. **The live block.** `draw_params_update` (0x0229e990, a large function of the scene renderer) blends each group's two entries and writes the result into the global block at 0x055406e0 (0x370 bytes, the "post effect parameters" below). It skips the writes while the debug menu holds the block (byte `+0x3200` of `[scene + 0x6078]`), so the debug menu's edits stay.
   | Group (bank, lookup, blend) | Fields |
   |---|---|
   | +0x5970, 0x021fe970, 0x00e83620 → 0x021d4d40 | the YEBIS setup flags `0x02`–`0x13`, `0x25`, `0xad`, `0xc0` |
   | +0x5978, 0x022044a0, 0x00e8a710 | auto exposure `0x14`–`0x24`, tone map `0x27`–`0x33`, `0x92`, chromatic aberration `0x2b`–`0x30`, vignette `0x34`, `0x35` |
   | +0x5980, 0x021e62d0, 0x00e6d790 → 0x021d5050 | bloom and glare `0x36`–`0x43`, `0x91`, `0xc1`–`0xd3` |
   | +0x5988, 0x021f68b0, 0x00e809e0 | light shafts `0x4c`–`0x5d`, `0x93` |
   | +0x5990, 0x021ef2e0, 0x00e75b50 | depth of field `0x62`–`0x76`, `0x90`, `0xb4`–`0xbf` |
   | +0x5998, 0x021e14c0, 0x00e6c5d0 | anti-aliasing `0x88` |
   | +0x59a0, 0x021fa460, 0x00e82290 | motion blur `0x77`–`0x81`, `0xab`, `0xac`, `0xd4` |
   | +0x59a8, 0x021f2b90, 0x00e788f0 | feedback blur `0x82`–`0x86` |
3. **The render command.** The post effect draw step (vtable 0x0535fe90, built by 0x021d1f80, which also fills the global block with its defaults through 0x021df510) copies the block into the frame's command buffer (0x021d5a80) or hands it on directly.
4. **YEBIS.** `pfx_apply` (0x021d5f80, `(self, context, block)`) runs on the render thread and calls a wrapper per YEBIS setter (0x00bb26e0–0x00bb5f10, each calls the `CPostEffect` method on the object at 0x05465ed0). YEBIS then draws the passes in its chain.

Field numbers in this note are dword indices into the block (byte offset = 4 × index). The debug menu that labels them is built by 0x021d9ea0 (its UTF-16 labels sit at 0x049c3c28 and on). Its "load" item (0x021d44f0) can read a 0x370-byte dump into the block.

## Motion blur

- **Fields.** `0x77` enable (1), `0x78` mode (2, velocity based; the apply passes 1 instead while bit 1 of the draw step's `+0x271c` is set), `0x79` blur time ratio (0.095), `0x7a` maximum blur length (0.2), `0xd4` motion blur scale (4), `0x7b` base samples (3), `0x7c` maximum recurrences (2), `0x7d` interleaved sampling, `0x7e`/`0x7f` camera rotation and translation thresholds, `0xab`/`0xac` falloff distances, `0x81` bidirectional.
- **Apply.** `SetMotionBlurEnable` (0x00bd3b00, through 0x00bb3870) gets `0x77`, or false when the block names no velocity texture (`+0x388`). With mode 2 the apply also registers the velocity source (0x00bb5430) with a length of `0x7a` × `0xd4`.
- **Writer.** The motion blur group, from the map's draw parameters.
- **Passes.** Three per frame in the clinic: one at 960×540 and two at 1920×1080 into RGBA16F (pixel shaders `d3c8bb21`, `e0305cef` twice, and `bf368417` in a pass it shares with `111fce32`).
- **Switch.** `0x77` = 0 in `pfx_apply`'s block removes the smear of a camera turn (clinic shot `02-turning`: the shelves' edges are sharp, the rest of the picture unchanged). Strength: `0x79` and `0xd4` scale the blur length (not tested).

## Chromatic aberration

- **Fields.** `0x2b` enable (1), `0x2c` wave samples (3), `0x2d`/`0x2e` lateral dispersion (0.006, 0), `0x2f`/`0x30` uniform dispersion (0, 0).
- **Apply.** `SetChromaticAberrationEnable` (0x00bd3320, through 0x00bb37a0) gets `0x2b`; 0x00bb37d0 passes `0x2c` to `0x30`.
- **Writer.** The tone map group.
- **Passes.** One full-size RGBA16F pass (pixel shader `3c2b7974`, which the glare chain also uses).
- **Switch.** `0x2b` = 0 removes the red and blue fringes, which grow towards the screen edges (Yharnam `00-start`: the carriage at the left edge loses its fringes; the probe's red-to-blue offset in the outer sixth of the shot drops from 3 px to 0). The picture gets sharper everywhere: the Laplacian variance of the clinic shots rises by 19 to 27 %, of the Yharnam shots by 40 to 70 %. Strength: `0x2d` scales it; at 0.003 (half) the clinic shots' sharpness lies between the default and off (181.6 against 168.6 and 214.6 in `01-clinic`).

## Depth of field

- **Fields.** `0x6b` enable (1), `0x6c` focus distance (1 m), `0x6d` aperture f-number (5.6), `0x70` sensor size, `0x62` auto focus (0), and the custom ranges `0xb4` enable (1), `0xb5`/`0xb6` near and far enable (1, 1), `0xbb`/`0xbc` far start and end (100 m, 150 m), `0xbd`/`0xbe` far circle of confusion.
- **Apply.** 0x00bb2960 sets the YEBIS depth of field enable (`CPostEffect +0x940`) from `0x6b`, or false when the block names no depth source (`+0x380`); the aperture, focus and edge quality setters run only when `0x6b` is set.
- **Writer.** The depth of field group.
- **Passes.** Three per frame, also when nothing is far enough to blur: two at 1176×441 (`265a6d2d`, `e80526cc`) and a full-size composite (`fbe7c4b6`).
- **Switch.** `0x6b` = 0 removes the passes; the shots of both routes stay within the run-to-run noise (mean difference 0.1 to 0.4 of 255 in Yharnam), since nothing on them lies beyond 100 m. A switch saves the passes but shows nothing on these routes; places with far views or cutscenes would.

## Bloom

- **Fields.** `0x36` glare shape (1), `0x37` glare luminance (1.64), `0x38` threshold (0.08), `0x39` bloom levels (8), `0x91` remap factor, `0x3b`–`0x40` star and ghost details, `0x41` anamorphic flare (0).
- **Apply.** `SetGlareParameters` (0x00bd4b60, through 0x00bb2840) gets `0x37`, `0x38`, `0x91`; 0x00bb40e0 sets the level count from `0x39`. The apply has no glare enable: the chain is set up at YEBIS initialization (`GlareQuality`, field `0x12`). Whether a glare shape of none (`0x36`) skips the chain was not checked.
- **Writer.** The glare group (0x021d5050).
- **Passes.** With the luminance at 0 YEBIS skips the whole glare chain: 33 passes per clinic frame (the B10G11R11F levels from 628×530 down to 5×5, and the 960×540, 480×270 and 314×265 RGBA16F sources).
- **Switch.** `0x37` = 0 removes the glow around lamps and lit windows (Yharnam `00-start`: mean difference 1.6 against 0.3 between two default runs, the brightest pixels darker, the picture 7 % sharper). Strength: `0x37` scales it (not tested at other values).

## Vignette

- **Fields.** `0x34` power (0), `0x35` field of view dependence (0).
- **Apply.** 0x00bb3670 → 0x00bd4f70: a power of 0 switches the vignette off (`CPostEffect +0x73c` = 0); the apply turns a negative power into −FLT_MAX.
- **State.** 0 in every block the probe logged, in the clinic and in Central Yharnam. Other maps' parameters may set it; a switch would write `0x34` = 0.

## Film grain and lens distortion

YEBIS has both (`DrawRectGPU_LensDistortionAndFilmGrain`, "image sensor noise"). Their setters, `SetLensDistortionEnable` 0x00bd2ab0 (`+0x958`) and the sensor noise setter 0x00bd2b80 (`+0x960`), have no caller in the eboot, and the function that sets up `CPostEffect` (0x00bc98b0) clears both flags. The game never draws them: no pass of the clinic frame changes with them, and no shot shows grain.

## The probe

A local probe build (not committed) wrapped `pfx_apply` and the draw step's execute (0x021d5a80): `BB_PFXLOG` logged the block every 300th call, `BB_PFX=index=value,…` overwrote fields of the block before every call, `BB_PFX_PERIOD_S=3` switched the overwrite on and off every 3 s and logged each change with its wall time. The pass probe of `docs/design/resolution.md` counted passes and shaders. Runs on 2026-11-20, `rig.py run bb-run --window`, 1280×720 window, shots by the rig's Vulkan layer.

Each run ended `scenario finished`. `clinic` is the title, Continue from the `r5-acceptance` save, a still shot, a shot 0.4 s into a full camera turn, and two more shots; `yharnam-route` is the R8.2 route from its own save.

| Run | What | Result |
|---|---|---|
| clinic, 9 variants | default, `0x2b`, `0x6b`, `0x77`, `0x37` off, `0x2d` halved | the shot differences above; two default-like runs (`0x71` and `0x73` changed, both depth of field rotation fields) give the noise: 0.1 to 0.2 mean difference (of 255) on `01-clinic` and `04-walked`, up to 0.85 on `03-turned`, where the camera settles a little differently each run |
| `yharnam-route`, 6 runs | default twice, `0x2b`, `0x77`, `0x6b`, `0x37` off | `00-start` is the same picture in every run (0.3 between the two default runs); the later shots end in one of the route's placement variants, so only same-variant shots were compared |
| clinic with the pass probe, 6 runs | default, each effect off, all four off | per frame 223 passes (default), 222, 220, 220, 190, 183 (all off); 93 dispatches in each |
| clinic, switched during play | `0x2b` and `0x77` off in alternating 3 s windows | the three shots inside an "off" window match the run with chromatic aberration off (0.1 to 0.7), the turn shot inside a default window matches the default run (0.1); the switch takes effect from the next frame without a reload |

The block holds the same values for these fields in the clinic and in Central Yharnam. Shots stay local (`analysis/issue-659/`).

## The switches (#665)

`bb-run` takes `--no-motion-blur`, `--motion-blur-strength S`, `--no-chromatic-aberration` and `--chromatic-aberration-strength S` (S from 0 to 1, a factor on the game's value), and the config keys `motion_blur`, `motion_blur_strength`, `chromatic_aberration` and `chromatic_aberration_strength`. The defaults leave the block as the game sets it.

- **Where.** A wrap of `pfx_apply` (0x021d5f80, `src/game/gfx/post_effects.cpp`) writes the fields into the block it receives, every call, and writes the game's values back after the original returns. One write before the game starts or to the global block does not hold (above). The write-back keeps the block the game's own: a strength scales the game's value and never the result of an earlier call, also if a block were handed over twice. The #659 probe saw a different block address every frame in the clinic and Central Yharnam: the frame's copy in its command buffer, which no other thread writes during the call.
- **Fields.** Off: `0x77` = 0 and `0x2b` = 0. Motion blur strength scales `0x79`, the blur time ratio, which YEBIS gets through 0x00bb38f0; the maximum length (`0x7a` × `0xd4`) stays the game's. Chromatic aberration strength scales the four dispersions `0x2d` to `0x30`, which 0x00bb37d0 passes on.
- **Tests.** `tests/game/post_effects_test.cpp` checks that each switch writes only its fields and that the wrap hands the edited block on and restores it, and, against the eboot, that `pfx_apply` reads exactly these fields for the effect's setters (the `cmp` and `vmovss` at 0x021d7178, 0x021d7193, 0x021d7d2e and 0x021d7f5f, and their calls) and that the wrap's prologue is the function's.
- **Check.** Runs on 2026-11-20, `rig.py run bb-run --window`, the clinic scenario of #659 and the first part of `yharnam-route` with a shot during the camera turn (local scenarios), each switch next to the default:
  | Shot | Default | Motion blur off | Chromatic aberration off | Half strength |
  |---|---|---|---|---|
  | clinic `01-clinic`, sharpness | 168.0 (168.7 in a second run) | 169.6 | 215.1 | aberration 182.8 |
  | clinic `02-turning`, sharpness | 109.1 (109.0) | 152.6, the smear of the turn gone, the picture otherwise the same | wall variant, not compared | blur: wall variant, not compared |
  | Yharnam `00-start`, red-to-blue offset at the edges | 3 px (3 px) | 3 px | 0 px, sharpness 509 → 862 | blur: 3 px |
  | Yharnam `02-turning`, sharpness | 260.3 (262.5) | 389.8 | 317.2 | blur: 335.1 |
  With motion blur off every still shot stays within the run-to-run noise (mean difference 0.1 to 0.4 of 255, 1.05 on the clinic's `03-turned`, where the camera settles differently each run). Values in brackets are a second default run. `rig.py compare main-menu` at the default against the two reference runs: ratio to the noise floor 1.019 and 1.007 for this branch, 1.019 and 0.982 for `main`, all within it.

## Anisotropic filtering (#658)

`bb-run --anisotropy 1|2|4|8|16` and the config key `anisotropy` set the anisotropy of samplers with linear magnification and minification and mipmaps (a mip filter and a LOD range). Point-filtered samplers, those without mipmaps and depth-compare samplers keep the game's value, and so does every sampler by default (`game`). 1 turns anisotropic filtering off on the samplers it reaches. The choice is `VideoCore::ChooseAnisotropy` (`src/shadps4/video_core/texture_cache/sampler_anisotropy.h`), clamped to the device's `maxSamplerAnisotropy`.

- **What the game asks for.** A probe of every new sampler in the clinic (20 in all) found three that the option reaches: the main material sampler, which the game already sets to anisotropic at 4x, and two trilinear samplers without it. One more trilinear sampler asks for a ratio of 16 but with a non-anisotropic filter, so it gets none from the game. The game does not ask for little anisotropy on its main materials, so 16x gains less than the issue expected.
- **Check.** Runs on 2026-11-20, `yharnam-route` at 60, three runs each at the default and at 16, and one at 1. `00-start` has the same placement in all seven runs. Mean edge strength of the far ground: default 16.64 to 16.67, 16x 16.65 to 16.79, 1x 16.11. Walls: default 21.44 to 21.47, 16x 21.50 to 21.53, 1x 21.06. So 1x blurs the street clearly, and 16x is a small gain beyond the run-to-run spread. By eye, the cobbles near the carriage on `02-camera` (the one placement that a default, a 16x and the 1x run share) are blurrier at 1x, and slightly crisper at 16x than at the default. No other part of the picture changes.
- **Cost.** At 60, every play segment holds a p50 of 16.69 to 16.71 ms and a p95 of 16.70 to 18.71 ms on both sides, within the spread. At 144, three runs per side are bimodal (7 or 14 ms a frame) with a spread of 5 to 9 ms, and the load average was 3.5 to 5, so they resolve no difference. The rig has no GPU timestamps.
- **Validation and the menu.** A clinic run with `--validation` at 16 reports no sampler message. Its two VUIDs, `RuntimeSpirv-OpEntryPoint-08743` and `VkBufferCreateInfo-size-06409`, are in earlier `main` runs too. `rig.py compare main-menu` of this branch at the default against two runs of `main`: 0.0267 against a noise floor of 0.0271.

## Shadows (#661)

Where the engine sizes its shadow maps and sets the cascade distances, and what twice the size costs and shows. The shadow map is one of the fixed-size targets of `docs/design/resolution.md`.

### Result

| Question | Answer |
|---|---|
| Targets | the sun's shadow map: one D32 depth target of 4096×4096 in the clinic and in Central Yharnam, an atlas of four cascades of 2048×2048 (2×2). Besides it, D32 targets of 256×256, 64×64 and 32×32 for point light shadows (the cube faces #660 found at 0x022b5b10); this issue did not size those |
| Where the size comes from | every frame, from the map's draw parameters: a size index in the shadow group picks one of four sizes from a table at 0x048a9668 (2048, 1024, 2048, 4096); an index of 4 or more gives 2048. Both maps use index 3 |
| Cascades | four, fixed. Split distances 2, 8 and 15 m in both maps, from the same group, clamped to ascend; the near plane is 0.1 m |
| Twice the size | the table times two gives an 8192×8192 target with four 4096×4096 cascades. It runs without a fault, but the shots show no difference beyond the run-to-run noise: the game draws its shadows through a soft, blurred screen-space pass, which hides the texel size |
| Cost | 192 MiB more of `GFX_GraphicsPrivate` (256 MiB for the 8192² D32 target against 64 MiB). Frame time at 144: at most about 0.3 ms, at the edge of the spread; at 30 none |

### The code

- **Scene renderer setup.** 0x022979b0 builds the scene renderer's description and calls its init 0x022c39f0: the frame buffer size at `+0`, a shadow map size of 0x800 at `+8` (the immediates at 0x02297b22 and 0x02297b2d). The init passes that size to the shadow scene (`scene + 0x1320`, init 0x00eabf20), which creates the depth target through 0x00eb0b30 (format 0x28, D32, one mip) and four cascade objects (`+0x130` to `+0x148`) that share it. This first size never reaches the GPU: the first frame's update replaces it.
- **Per frame.** 0x0229cb70 calls the shadow scene's update 0x00eac210 with the map's shadow group (`+0x80` of its draw parameters, blended like the post-effect groups by 0x0229e990). The update reads the size index at group `+0x90`, looks the size up in the table at 0x048a9668 (0x800 for an index of 4 or more), and when it differs from the current size (`+0x860`, `+0x864`) it calls the init 0x00eabf20 again, which frees the old target and creates the new one.
- **Distances.** The same update copies group `+0x34` (near, 0.1) to `+0x6d0` and the three splits `+0x40`, `+0x44`, `+0x48` (2, 8, 15 m) to `+0x6dc`, `+0x6e0`, `+0x6e4`, each raised to at least the previous one plus 0.1 m, and `+0x38` to `+0x6d4`, raised to at least the last split plus 0.1 m. A local probe logged the group on every change: from a load on, `+0x38` and `+0x3c` blend over about 70 frames to their map values (2 and 10 in the clinic, 2 and 15 in Central Yharnam); the index and the splits stay. #660's camera probe saw the cascade slices 0.1–2, 2–8, 8–15 and 15 to 25.1 or 40 m, so the end of the last slice comes from elsewhere (not traced).
- **Cascade viewports.** 0x00eacb30 places cascade i at x = (i & 1) × size / 2 and y = (i ≥ 2) × size / 2, each size / 2 square. The pass probe of `docs/design/resolution.md` saw this in the clinic: four of the five logged frames drew only the two cascades of the top row (44 draws), one drew all four (74 draws, 104 in the run at twice the size).
- **The screen-space pass.** A full-size pass (pixel shader `f55643de`) reads the shadow map and the depth, and writes the shadow mask that lighting uses. The debug menu of the shadow parameters (0x00eb3250, labels at 0x048a9b50 and on) names its modes `GXScreenSpaceCascadedShadow_PCF4`, `_PCF9` and `_Soft`, and a filter mode, blur count and blur radius. That filtering is why a sharper map shows so little; whether a harder filter would show the larger map was not tested.

### The probe and the runs

A local probe build (not committed): `BB_SHADOW=2` multiplied the four table entries before the game starts, `BB_HEAPLOG` logged the free space of `GFX_GraphicsPrivate` and `GFX_GraphicsPrivateB` every second (as in #640), a wrap of 0x00eac210 logged the shadow group on every change, and the pass probe of #511 logged the targets. Runs on 2026-11-20, `rig.py run bb-run --window` (1280×720 window, 1920×1080 frame), the clinic scenario of #659 and the first part of `yharnam-route` (the #665 local scenario). Every run ended `scenario finished`, with no crash or GPU fault.

| Runs | Default | Twice |
|---|---|---|
| Shadow target (pass probe, clinic) | 4096×4096, viewports 2048×2048 | 8192×8192, viewports 4096×4096, 44 draws in the two-cascade frames as at the default |
| Least free in `GFX_GraphicsPrivate`, clinic (3 + 3) | 408 MiB | 213 MiB |
| Least free, Yharnam at 30 (3 + 3) | 343 to 350 MiB | 149 to 153 MiB |
| Least free, Yharnam at 144 (3 + 3) | 463 MiB | 268 MiB |
| `GFX_GraphicsPrivateB` | unchanged (188 and 133 MiB) | unchanged |
| Frame time, Yharnam at 30 (3 + 3), p50 / p95 in every play segment | 33.33 / 33.35 to 33.46 ms | 33.33 / 33.35 to 33.37 ms |
| Frame time, Yharnam at 144 (3 + 3), p50 in `play-stand` / `play-walk` / `play-camera` | 7.08 to 7.28 / 6.95 to 7.15 / 6.95 to 6.96 ms | 7.15 to 7.31 / 7.16 to 7.46 / 6.95 to 6.96 ms |
| Shot difference, mean of 255 | clinic `01-clinic` 0.07 between two defaults; Yharnam `00-start` 0.26 to 0.47 | 0.12 to 0.19 against a default; 0.35 to 0.49 |
| Edge strength in Yharnam `00-start` (mean gradient), the wheel's shadow on the cobbles / the street at the right / the ground in the centre | 8.857 to 8.869 / 7.542 to 7.551 / 15.266 to 15.268 | 8.859 to 8.876 / 7.539 to 7.549 / 15.270 to 15.271 |

The 144 runs are bimodal (7 or 14 ms a frame, p95 14 to 14.7 ms on both sides), and the machine had a load average of 3.4 to 5.6 from other programs during them, so they bound the cost at about 0.3 ms without resolving it. In the clinic the larger differences sit on the hunter, but they are as large between the two runs at twice the size (0.70 in a box around the hunter) as between the sides: the idle pose, not the shadow. Shots stay local (`analysis/issue-661/`).

### What an option needs

A larger size works without a fault, so an option is possible (#718): multiply the table at 0x048a9668 before the game starts, rather than poke the immediates in 0x022979b0, which the first frame overrides. It must grow `GFX_GraphicsPrivate` by the target's extra bytes (192 MiB at twice the size; four times would be 960 MiB, 16384² D32) the way `--resolution` does (`docs/design/resolution.md`), or a large frame buffer and a large shadow map together run the heap dry. At 1920×1080 it shows nothing on these routes. Where it could show is above 1080p, where a cascade texel covers more pixels, and with a harder screen-space filter. The option issue's check has to find such a place first; it is low priority.

## Not switched yet

- Bloom (`0x37` = 0, or scaled) and depth of field (`0x6b` = 0) are clean as well. Depth of field changes nothing on the two routes, so it needs a check in a place with a far view before it becomes an option.
- The vignette is off on both routes; film grain and lens distortion are never drawn.

## Field of view (#666)

`bb-run --fov DEGREES` and the config key `fov` set the vertical field of view of the player's camera, from 30 to 65 degrees, in place of the game's 43. 63 is about 95 degrees horizontal at 16:9. The default leaves the camera as the game sets it.

- **Where.** In the wrap of the chase camera's update (0x0143ac60, `src/game/camera/chase_cam.cpp`, `field_of_view.cpp`). After each update the chase camera's `+0x50` gets the game's value plus the option's difference to 43 degrees; before the next update the game's value goes back. So the update's ease toward the parameter row, its collision and its framing (the reads at 0x0143af5e, 0x0143bbb0 and 0x0143bf04) see the game's angle, and the widening never adds up. A value that something else writes between updates is kept as the game's. The camera manager copies `+0x50` right after the update (0x01436c05, the blend at 0x01436e38) and passes it to the render camera, so the projection, the culling, the shadow cascades and YEBIS see the wider angle. Lock-on uses the same camera. Cutscenes use cameras of their own and keep their angles.
- **Range.** Rows other than the default (38 to 48 degrees) keep their difference to 43, so at 65 a 48-degree row shows 70. The runs below checked 70 with no fault; #660 saw streaks over the top of the picture at 77.
- **Tests.** `tests/game/field_of_view_test.cpp` runs the widener around a stand-in for the update that eases `+0x50` as the game does: the update always sees the game's angle, the field holds it widened, a value written between updates and a new camera are taken as the game's. Against the eboot it checks the constructor's 43 degrees and the update's own load and store of `+0x50`.
- **Check.** Runs on 2026-11-20, `rig.py run bb-run --window`:
  | Run | Result |
  |---|---|
  | `clinic-fight` at 63, against the default | more of the room on every side in `00-clinic`, `01-attack` and `02-lockon` (the lock-on view is wider too), no object missing at the edges, the HUD unchanged |
  | `clinic-fight` at 70 (the most a 48-degree row reaches at 65) | wider still, no streaks and nothing missing at the edges |
  | `yharnam-route` at 63, against the default | `00-start` and `01-street` show more of the street, the carriage whole, the HUD unchanged |
  | new game and intro (the #660 local scenario) at 63, against the #660 default run | the seven cutscene shots keep their framing (mean difference 0.2 to 2.5 of 255, timing); the clinic after the cutscene is wider (7.5) |
  | `rig.py compare main-menu` at the default, against the two reference runs | 1.007 of the noise floor, `main` 0.997 |
  | `clinic-fight`, three runs each at the default and at 63 (30 FPS) | every play segment p50 33.33 ms and p95 33.36 to 33.48 ms on both sides: the cap hides the cost. CPU per frame of the render threads (`thread_cpu.py`) rises at 63 by 0.4 to 0.7 ms for the GX workers and 0.3 to 0.5 ms for the GPU thread in the moving segments, above the spread of 0.03 to 0.7 ms; the game thread by about 0.2 ms. The machine had a load average of 2.3 to 2.7 from other sessions |

## Wide frame buffer (#667)

`bb-run --resolution` and the config key `resolution` (#668) take the wide sizes 2560x1080 and 3440x1440 (21:9) and 5120x1440 (32:9) besides the 16:9 sizes. The frame buffer is set as at 16:9 (`docs/design/resolution.md`), the graphics heap grows with the pixels as there (5120×1440 is below 3840×2160), and the projection gets the frame buffer's own aspect, so a wide frame shows more at the sides with the vertical angle unchanged (Hor+).

- **Where.** A wrap of the render camera copy 0x01d16690 (`src/game/camera/aspect.cpp`): after the game's copy, a render camera aspect `+0x64` that holds the game's 16:9 constant becomes width / height. Every game camera passes this copy (chase, lock-on, cutscenes), so the projection, culling, the shadow cascades and YEBIS follow, as #660's probe showed. Other aspects stay: the character editor's square preview (1) and the render camera's own width / height (0). At 16:9 sizes the wrap changes nothing, also at sizes where width / height does not round to the game's constant. The render camera's width and height fields keep the game's 1920×1080. The projection takes the aspect from them only when the aspect is 0. It also uses them for a pixel offset row, which stays in 1080p pixels, as at the 16:9 sizes above 1080p.
- **Sizes.** Only the three sizes checked. The width 3440 is not a multiple of 64 like the 16:9 sizes'; the half and quarter targets still have whole sides (1720, 860).
- **Markers in the world.** The lock-on reticle and the enemy health bars come from three projections with the game camera's own 16:9 aspect (0x016439b0, 0x016448f0, 0x01644cf0). That aspect is what the HUD's 16:9 stage needs, so the wrap leaves them alone. But they scale to the frame buffer's pixels, which the HUD reads as stage coordinates, so above 1080p (16:9 or wide) the markers sat off their target. Since #713 these loads, and the checks of their readers against the size (0x01654d80, 0x016b3320, 0x01bfb2a0), read the stage's 1920x1080 at any other size (`src/game/gfx/hud_stage.hpp`). Giving the camera the screen's aspect for the three calls did not help: the stage is 16:9, centred on a wider screen, and needs the 16:9 projection. At a wide size a marker beyond the stage's sides counts as off screen, as on the PS4.
- **HUD and menus.** They need nothing: the game's HUD keeps its 16:9 layout in the centre (#660, "The HUD"), and Scaleform fits its 16:9 stage into the frame. The sides beside the full-screen menus are black since #711 (below).
- **Tests.** `tests/game/aspect_test.cpp`: the aspect for 16:9 and wide sizes, the replacement of 16:9 only, and against the eboot the game's copy under the wrap with the constructors' constant.
- **Check.** Runs on 2026-11-20, `rig.py run bb-run --window` (1280×720 window, so a wide frame is a band across it):

  | Run | Result |
  |---|---|
  | clinic at 2560x1080 and 3440x1440 (the #660 local scenario) | correct proportions, more of the room at the sides, nothing missing at the edges, the HUD at its 16:9 size in the centred area |
  | clinic at 5120x1440 | the same at 32:9 |
  | clinic at 5120x1440 with `--fov 65` (about 132° horizontal) | strongly stretched at the sides as any projection this wide, but no streaks and nothing missing at the edges |
  | lock-on in Central Yharnam (local scenario on the `yharnam-fight` save) at 1920x1080, 2560x1440, 2560x1080 and 5120x1440 | before #713 the reticle and the enemy bar sat on the target only at 1920x1080; with the stage loads they sit on it at all four sizes |
  | `main-menu` at 2560x1080 and 3440x1440 | logos and title menu 16:9 in the centre, not stretched, with grey sides (black since #711); the title menu takes input as at 16:9 |
  | `yharnam-route` at 2560x1080 and 3440x1440 | the street wider, the carriage and the statues whole at the edges, the HUD in the centre; peak memory 4.0 and 4.0 GB |
  | `clinic-fight` at 3440x1440, two runs (30 FPS) | every segment p50 33.33 ms and p95 33.35 to 33.36 ms (35.4 in `play-camera`), no stutters after the logos, peak memory 3.2 GB; no crash or GPU fault |
  | `clinic-fight` at 3440x1440 with `--validation` | finished; the validation layer reports only the two kinds a 1920x1080 run of 2026-11-20 (`clinic-floor`) reports too: `VUID-RuntimeSpirv-OpEntryPoint-08743` (22) and `VUID-VkBufferCreateInfo-size-06409` (the 4 GiB buffer at start) |
  | `rig.py compare main-menu` at 1920x1080, against the two reference runs | 0.997 of the noise floor (`main` 1.007): 16:9 unchanged |

  At a wide frame `clinic-fight` reaches the title menu but sends its menu inputs too early: the first logo changes over a smaller share of the shot, so `stable delta=1` already holds on it. The runs above used a local copy with 14 s more before that wait; the game's logos take no longer than at 16:9.

### Black sides beside the menus (#711)

At a wide frame buffer the logos, the title menu and the loading page sat in the centre with grey sides. Two sources, found with a local pass probe at 2560x1080 and a log of the movies the engine creates:

- **The device clear colour.** Every frame starts with a compute clear of the 2D target (2560x1080 RGBA8, the target the menus, the HUD and the world's tone-mapped picture are drawn into) to (0.5, 0.5, 0.5, 1). `graphics_init` (0x01d969f0) copies that colour from a 16-byte constant at 0x04914f00 into its device description (0x01d96afb, the constant's only reader). The logos and the title menu draw nothing over the sides, so the grey stays. At a size other than 16:9 bb-run writes (0, 0, 0, 1) there before the entry point (`set_device_clear_color`, `src/game/gfx/frame_buffer.cpp`). At 16:9 the colour stays the game's: the full-screen movies cover all of it, and the world draws over it in play.
- **The loading page.** While a map loads the engine keeps rendering the world under the loading page (the movie `NowLoading2`): grey while the map is still empty, then the room. The engine's movie holder (0x01f58360) gives every movie a viewport of the whole frame buffer and a transparent background colour (vcall `+0x108`, Scaleform's 0xAARRGGBB). A movie with an opaque background clears its whole viewport first, so a wrap of the holder sets opaque black for `NowLoading2` alone, at sizes other than 16:9 (`src/game/gfx/loading_page.cpp`). The other movies keep their transparent background: the HUD and the in-game menus show the world under them on purpose. That #667's viewport try changed nothing fits: the grey was never Scaleform's border.
- **Side effect.** At 16:9 the loading page fades out over the world. With the backdrop, at a wide size, it fades to black and the world then appears at once, a cut of a few frames instead of a cross-fade. The page is not drawn once it is gone, so the backdrop never covers play.
- **Tests.** `tests/game/loading_page_test.cpp`: which movie gets the backdrop at which aspect, the clear colour for each size, and against the eboot the holder's prologue and background call, `graphics_init`'s read of the constant, and the write of the constant with the segment staying read-only.
- **Check.** Runs on 2026-11-20, `rig.py run bb-run --window` (1280×720 window), a local scenario through the logos, the title menu, Continue and the loading page into the clinic. Every run ended `scenario finished`.

  | Run | Result |
  |---|---|
  | 2560x1080, 3440x1440 and 5120x1440 | the logos, the title menu and the loading page (shots at 2 s and 5 s) with black sides (the brightest side pixel 0), the centre as before; the clinic after the load fills the width |
  | 2560x1080, a shot every 0.3 s through the end of the load | the page fades to black, the clinic follows a shot later; before, the page faded out over the clinic already showing at the sides |
  | `rig.py compare main-menu` at 1920x1080, against two runs of `main` | 0.977 of the noise floor: 16:9 unchanged |

## Camera and aspect ratio (#660)

Where the engine sets the field of view and the aspect ratio of its projection, and how the HUD and menus are placed, for the field-of-view option (#666) and the wide frame buffer (#667). Addresses as above; `SprjPersCam` is the game's own class name (its runtime class is registered at 0x01d16e60).

### Result

| Question | Answer |
|---|---|
| Field of view | vertical, in radians at `+0x50` of every game camera. The chase camera eases it toward its camera parameter row's angle, clamped to 38°–48° (43° in the clinic); cutscenes set their own (32° to 44° in the intro) |
| Aspect ratio | a constant 16:9 at `+0x54` of every game camera, written by its constructor; not taken from `frame_buffer_width` / `frame_buffer_height` |
| Projection | 0x021aa290 builds it from the render camera: y scale cot(fov / 2), x scale y scale / aspect. The vertical angle is fixed, so a wider aspect shows more at the sides (Hor+) |
| Culling | no object missing at the screen edges at 56° and 77° or at 64:27: the same render camera feeds the projection, the shadow cascade fits and the other users below |
| Cutscenes | in-engine, the same cameras, 16:9, no letterbox of their own. At 2560×1080 with the aspect fixed they fill the frame |
| HUD and menus | at 2560×1080 the HUD keeps its 16:9 size and sits in the centred 16:9 area; full-screen menus, logos and loading screens are 16:9 with grey (128, 128, 128) bars at the sides (black since #711) |

### The game cameras

- **`SprjPersCam`.** The base constructor stores the vtable 0x0539ffb0 and the defaults (0x01d1661a–0x01d16644): `+0x10`–`+0x4f` the camera matrix, `+0x50` field of view 0.75049 rad (43°), `+0x54` aspect 1.77778 (`0x3fe38e39`), `+0x58` near 0.05, `+0x5c` far 3000.
- **The chase camera.** `chase_cam_construct` (0x0143a0e0) stores the same four values (0x0143a353–0x0143a368) and the vtable 0x0536b7c0. `chase_cam_update` (0x0143ac60) looks up its camera parameter row (0x0143add1–0x0143ae62) and eases `+0x50` toward the row's angle at `+0x14`, in degrees: below 38 it takes 38 (0.663225 rad, 0x04925e5c), above 48 it takes 48, then × π/180 (0x0143af14–0x0143af70), by the factor `+0x330` per call. It reads `+0x50` again for its collision and distance (tan(fov / 2) at 0x0143bbb0 and 0x0143bf04).
- **Other cameras.** 27 stores of the 16:9 constant to `+0x54` in camera constructors and setup code: 0x01432bc0, 0x014332a0, 0x01435fc0 (3), 0x0143a0e0, 0x0156fec0 (4), 0x01973df0, 0x0199e550, 0x019a5d60, 0x019ba0a0, 0x01a07f10 (5), 0x01da7aa0, 0x01edc310 (2), and five outside the functions Ghidra found (0x014312a2, 0x01444509, 0x014462b9, 0x01d14569, 0x01d1662c). No code reads the frame buffer size for an aspect: the #660 sweep of all readers of 0x051289f8 / 0x051289fc found no ratio.

### The render camera

`SprjPersCam`'s vtable slot `+0x28` (0x01d16690; the chase camera's vtable has the same slot) copies a game camera into a render camera. The callers are 0x01edc310 (twice) and 0x01da7aa0. The render camera's layout:

| Offset | Field | Set by 0x01d16690 |
|---|---|---|
| `+0x00`–`+0x3f` | camera matrix | game camera `+0x10` |
| `+0x50` | field of view (vertical, rad) | `+0x50` |
| `+0x54`, `+0x58` | near, far | `+0x58`, `+0x5c` |
| `+0x5c`, `+0x60` | width, height (u32) | the constants 1920 and 1080 (0x01d166d5) |
| `+0x64` | aspect; 0 means width / height | `+0x54` |
| `+0x68` | half-pixel offset flag | — |
| `+0x70`–`+0x80` | tile count, index and sizes (tiled capture) | — |

The projection builder 0x021aa290 (`(out, camera)`) and the tile frustum 0x021aa950 read this layout; 0x021ab450 fills it from a packed copy with 16-bit tile fields. The probe saw these callers of 0x021aa290 in the clinic and the intro: 0x02290dd0 (the view state: view matrix at `+0x10`, projection at `+0x50`; also called with the shadow cascade slices, near and far 0.1–2, 2–8, 8–15, 15–25.1 or 40 m), 0x021aaa90, 0x02192d20, `pfx_apply` 0x021d5f80, 0x022d46b0, 0x00e669d0, 0x00eed3c0, 0x00ef1c00, all with the camera's angle and 16:9, and 0x022b5b10 with 90° and an aspect of 0 at 32 to 256 pixels square (point light shadow cube faces). The character editor's preview uses its own 784×784 camera with aspect 1 (49°, and 23°). 0x021aa950 had no caller in these runs.

### The HUD

- The game's own HUD scales its rectangles by `S × 1.5 × x × 0.125` with `S` the factors width / 1920 and height / 1080 at 0x055404d0 / 0x055404d4 (0x01816a50, 0x01817690, 0x018179c0, 0x0181a9c0); `system_properties_init`, the frame buffer setter 0x01d9e850 (vtable 0x0534dcb8) and 0x0201a2f0 compute them.
- The pointer slots (`[0x05562878] + 0x10f8 + i × 0x98`, 8 slots) count as valid only inside 0 ≤ x < width and 0 ≤ y < height (0x01654d80, 0x016b3320, 0x01bfb2a0); 0x01654d80 centres the cursor at width / 2, height / 2.
- 0x01f58360 gives each Scaleform movie a viewport of the full frame buffer (vcall `+0x68`); the movies then fit their 16:9 stage inside it.
- 0x016439b0, 0x016448f0 and 0x01644cf0 map projected points to pixels with the full width and height (markers in the world, likely the lock-on).

At 2560×1080 the shots show the HUD bars and counters at their 1080p pixel size, 320 px further right than at 1920 wide, so the HUD stays inside the centred 16:9 area; the subtitles stay centred. Forcing the width factor to 1.0 left the title menu unchanged; the HUD was not shot with it. Whether some element the clinic does not show (the lock-on marker, item pick-up messages, the boss bar) uses the full width was not checked.

### What the options need

- **Field of view (#666).** #660 proposed scaling `+0x50` of the render camera that 0x01d16690 writes when its source is the chase camera. That source is never the chase camera: the camera manager copies the chase camera into a `SprjPersCam` (vtable 0x0539ffb0) first, as it does with the other cameras, so at 0x01d16690 they look the same. #666 widens the chase camera itself instead (below).
- **Wide frame buffer (#667).** `--resolution` with a width / height other than 16:9 (the global write works as at 16:9), plus `+0x64` of every render camera that 0x01d16690 writes set to width / height; without it the picture is the 16:9 projection stretched to the width. The HUD and the menus need nothing to keep their 16:9 size; the grey side bars of the full-screen menus and loading screens are the open question (black, or the menus' background widened). Done as planned: "Wide frame buffer (#667)" above.

### The probe

A local probe build (not committed) wrapped 0x021aa290 and 0x021aa950: `BB_CAMLOG` logged the render camera's fields per caller, `BB_FOVX=k` multiplied `+0x50` by k during the call for every camera with an aspect (so not the shadow cubes), `BB_ASPECT=a` set `+0x64`, and `BB_HUDX` held the HUD width factor. `--resolution` took 2560x1080 with its 16:9 check removed. Runs on 2026-11-20, `rig.py run bb-run --window`, 1280×720 window, so a 2560×1080 frame is a 1280×540 band in the shot. Every run ended `scenario finished`.

| Run | What | Result |
|---|---|---|
| clinic, 1920×1080 | default | 43° and 16:9 in every render camera, cascade slices as above |
| clinic, `BB_FOVX=1.3` (56°) | wider view | more of the room on every side (the lamp stand and the lit door at the right), the HUD unchanged, no object missing at the edges |
| clinic, `BB_FOVX=1.8` (77°) | stress | still nothing missing at the edges, but vertical streaks over the top fifth of the picture, which 56° does not show (cause not looked for; an option should stay well below this) |
| clinic, 2560×1080 | the game's 16:9 | the 16:9 picture stretched to the width by 4:3 (the hunter visibly wider) |
| clinic, 2560×1080, `BB_ASPECT=2.370370` (64:27) | Hor+ | correct proportions, more of the room at the sides, nothing missing at the edges, the HUD at 16:9 size in the centre |
| new game and intro, 1920×1080 | cutscene | its own angles: 44°, 43°, 41.3°, 35.6°, 31.9°; 16:9, full frame |
| new game and intro, 2560×1080, `BB_ASPECT=2.370370` | cutscene | fills 21:9 with the same vertical framing, subtitles centred; the editor and menus at 16:9 with grey sides |
| title, 2560×1080 | menus | the logos run longer, so a scenario must wait for them by picture, not by a fixed step; the menus then take input as at 16:9 |

Shots stay local (`analysis/issue-660/`).

## Draw distance (#662)

What limits how far the game draws and in how much detail, for a draw-distance option. Addresses as above.

### Result

| Parameter | Where | Value | Larger |
|---|---|---|---|
| Model LOD distances | LOD_BANK rows (game parameters), picked per model in `0x01d42ef0` | d01 / d12 pairs seen in Central Yharnam: 5 / 10, 10 / 15, 10 / 20, 15 / 30, 40 / 1000, 1000 / 2000, with 1 to 2 of hysteresis | 1.5× and 2× run without a fault. More models keep a finer mesh (below), but the shots match the default within the run-to-run spread, and the cost is within the spread too. Option: #720, low priority |
| LOD scale | view `+0x150`, written every frame by `0x02291c30` | tan(fov / 2) / tan(27.5°) × 720 / 1080: 0.504 at 43°, 0.785 at `--fov 63` | the knob for the option: the distance is multiplied by it, so dividing it lengthens every LOD distance at once |
| LOD bias | global 0x05587d20, copied every frame by `0x01d41d60` from the render manager's `+0x530` | 0 in these runs (not logged; read from the code) | added to every model's level before the 0..2 clamp; −1 would draw one level finer everywhere. Not tried |
| Far plane | game camera `+0x5c`, render camera `+0x58` (#660) | 3000 m | 2× changes nothing in `yharnam-route`: nothing there is that far. No option |
| Character update interval | WorldChrManDbg (0x0553e880) `+0x2c`, read in WorldChrMan's frame update `0x01915320` | 40 m: beyond it from the player a character updates every 30 frames, within it every 5, the nearest every frame (counts `+0x24` = 10, `+0x28` = 20) | 80 m ran without a fault but showed nothing in the shots (no enemy near the route). The interval is also a level of detail the characters' own code tests (`docs/design/frame-rate.md`), so a longer distance changes behaviour, not only looks. No option |
| Shadow distance | the shadow group (#661): cascade slices end at 25 to 40 m | see "Shadows (#661)" | not tried here |
| Grass and effect fades | no global distance found. Central Yharnam's weeds are map models with LOD_BANK rows; effects carry their own distance curves (`FXEvaluatableDistanceFromCamera`) | — | not tried |

Nothing in this path hides a model by distance: the three levels are three meshes, and the lowest still draws. If the game hides models by distance anywhere, it is outside this path; this issue found no such distance.

### The code

- **Entries.** `CSLod` (singleton 0x05540080, update `0x01d41d60` from `0x01d924c0` every frame) holds a 0x90-byte entry per model draw entity: the level at `+0`, the LOD_BANK row at `+8` (four floats: d01, its hysteresis, d12, its hysteresis), an oriented box at `+0x10` to `+0x5f`. `0x01d42440` allocates one and `0x01d42aa0` looks up the row by id in the LOD_BANK parameter (registered with `LodBankMan`, singleton 0x055402d8, at `0x01f112d0`). Three FLVER draw entities make entries: `0x01d1a150` and `0x01d232b0` (likely map models, from their callers and debug names; the id is a byte at `+0x1b` of their part data; start at level 0) and `0x01d20b40` (the draw entity of a `CSModelIns`, a model instance; id at `+1`; starts at level 2). Which entity draws which kind of model was not checked at run time.
- **Selection.** `0x01d42ef0` takes the distance from the camera position (view `+0x80`) to the entry's box, times the view's `+0x150`. From level 0 it moves to 1 beyond d01 + h01 and to 2 beyond d12; from 1 back to 0 below d01 − h01 and on to 2 beyond d12 + h12; from 2 back to 0 below d01, else to 1 below d12 − h12. At the 0.504 scale a row's distances are about twice as far in metres. `CSLod` also sorts the entries into distance buckets that it updates at different rates (`0x01d43c60`, `0x01d43f10`).
- **Use.** The draw submits read the level, add the bias and clamp to 0..2: `0x01d181c0` (at 0x01d1875f), `0x01d24660` (0x01d2487e) and `0x01d6f9d0` (0x01d6fc09, where level 2 takes another resource set). The callback at `+0x60` of an entry is called on a change but never set.
- **The scale.** `0x02291c30` builds the view from the render camera: `+0x150` = tan(fov × 0.5) / tan(0.47995) / (`+0x114` / 720), with `+0x114` the view's height in pixels, 1080 at 1920×1080 and still 1080 at `--resolution 2560x1440` (the logged scale stayed 0.504; the render camera keeps 1920×1080, #667). So a higher resolution does not lengthen the LOD distances, and a wider `--fov` shortens them.

### The probe and the runs

A local probe build (not committed): a wrap of `0x01d42ef0` counted the levels once a second and logged each new LOD_BANK row and the scale (`BB_LODLOG`); `BB_LODK=k` divided view `+0x150` by k for the call; `BB_FARK=k` multiplied the render camera's far plane in the wrap of `0x01d16690`; `BB_CHRDIST=k` multiplied WorldChrManDbg `+0x2c` once. Runs on 2026-11-20, `rig.py run bb-run --window` (1280×720 window, 1920×1080 frame), `yharnam-route` and its first part up to `01-street`. Every run ended `scenario finished`, with no crash or GPU fault.

| Runs | Default | 1.5× | 2× |
|---|---|---|---|
| Level updates a second, mean over the route (3 + 2 + 3 runs): level 0 / 1 / 2 | 730 to 802 / 415 to 423 / 1275 to 1305 | 4663 to 4695 / 3854 to 3863 / 882 to 885 | 7961 to 8104 / 7857 to 8013 / 660 to 669 |
| Shot difference against the first default run, mean of 255, `00-start` / `02-camera` | 0.32 to 0.53 / 0.11 to 0.19 | 0.52 to 0.53 / 0.09 to 0.12 | 0.33 to 0.43 / 0.18 to 0.19 (one run's `02-camera` landed elsewhere: 7.8) |
| Frame time at 144 (3 + 3, first part), p50 `play-stand` / `play-walk` | 6.96 to 7.27 / 7.28 to 7.51 ms | — | 6.98 to 7.01 / 7.11 to 7.36 ms |
| CPU per frame of the GX workers in `play-walk` (`thread_cpu.py`) | 8.36 to 8.61 ms | — | 8.44 to 8.54 ms |

- The later shots of `yharnam-route` (`01-street`, `03` to `06`) differ by 0.2 to 38 between two runs, defaults included: the walk ends in one of two places (#591). Only `00-start` and, in most runs, `02-camera` land in the same place.
- More updates a second at 1.5× and 2× because entries move into the nearer, more often updated buckets; it costs no measurable CPU.
- Forcing the coarsest level (`BB_LODK=0.02`) shows what the levels are: the carriage beside the start loses its gold trim and wheel detail, `00-start` differs by 4.3 (against 0.3 to 0.5 between defaults).
- `--fov 63` logged a scale of 0.785, `--resolution 2560x1440` 0.504.
- `BB_FARK=2` and `BB_CHRDIST=2`: `00-start` 0.44 and 0.39 against the default, within the spread.
- The 144 runs had a load average of 3 to 6 from other programs; they bound the cost, they don't resolve it.

Shots stay local (`analysis/issue-662/`).

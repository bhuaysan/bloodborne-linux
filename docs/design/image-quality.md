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

## For #665

- Hook `pfx_apply` (0x021d5f80, a wrapper with the prologue `push rbp; mov rbp, rsp; push r15–rbx; and rsp, -0x20`, 20 bytes) and write the chosen fields into its third argument before calling the original. Every frame passes through it; one write before the game starts or to 0x055406e0 does not hold.
- `--no-motion-blur`: `0x77` = 0. `--no-chromatic-aberration`: `0x2b` = 0, with a strength through `0x2d`. Both clean.
- Bloom (`0x37` = 0, or scaled) and depth of field (`0x6b` = 0) are clean as well. Depth of field changes nothing on the two routes, so it needs a check in a place with a far view before it becomes an option.
- The vignette is off on both routes; film grain and lens distortion are never drawn.

# Launcher (R9)

The launcher `bb-launcher` sets up the game and starts it. It's for players who have never opened a terminal: they choose the game folder of their own dump once, then press Play. The roadmap's R9 entry sets the scope: no PKG installs, nothing downloaded. This page records how the launcher looks and behaves, and why.

The guideline is: keep it simple, but don't look like every other launcher.

## One portable folder

The game is one folder that the player unpacks anywhere they can write. Nothing is installed, and there is no menu entry:

```
Bloodborne-Linux/
├── bb-launcher, bb-run
├── lib/        libraries not linked statically (RPATH $ORIGIN/lib)
├── game/       optional: the dump put here is found without setup
└── user/       config.toml, profiles/<name>/savedata/, cache/, logs/
```

Everything the launcher and `bb-run` write goes to `user/` (`docs/design/m3-core-os.md`, "Runtime directories"). Deleting the folder removes the game with its settings, saves, caches and logs; nothing stays in `~/.config`, `~/.local/share`, `~/.cache` or `~/.local/state`, and later nothing in AppData on Windows. The dump stays where the player keeps it, and only its path is stored. A dump inside the folder is stored relative to it, so the whole folder can be moved or renamed, for example onto a USB stick. We don't copy the dump in: it is large, and players keep it where they like (#728).

A folder that can't be written (copied to `/usr/bin`, say) gets a status line that says so and asks to unpack the game into a folder of its own.

What we can't control: the folder dialog runs through xdg-desktop-portal, which may add the chosen folder to the desktop's "recently used" list (dragging the folder onto the window doesn't); a crash may leave a core dump with systemd-coredump; a graphics driver that ignores the cache variables writes its cache where it likes.

## Layout: one window in the style of a late-90s autorun menu

A small window with a fixed size, the game's title picture at the top, a one-line status below it, then a single column of buttons. The first button is Play, and it is bigger than the others. Late-90s PC games opened with a menu like this. It needs no explaining: there is one obvious button, and everything else is a short list under it.

```
+--------------------------------+
|                                |
|   title picture (pic1.png)     |
|                                |
+--------------------------------+
|  ok  Version 1.09 . controller |   status line
+--------------------------------+
| [          Play              ] |   bigger, focused at start
| [     Picture and sound      ] |
| [         Saves              ] |
| [    Help with problems      ] |
| [         Quit               ] |
+--------------------------------+
```

- **Size:** 480 × 600 logical pixels, not resizable, scaled with the display's content scale (`SDL_GetDisplayContentScale`). The pages behind the buttons open in the same window, in the area of the button column, with a Back button. No second window.
- **Title picture:** `sce_sys/pic1.png` from the player's game folder (1920 × 1080 in the dump), cropped to the header's aspect ratio around its centre. It is loaded at run time and never copied. We ship no game art, and every install still shows the game. Before the game folder is set up, the header shows the drop target instead (see First start).
- **Window title:** the game's title from `sce_sys/param.sfo`, read at run time, plus "(bloodborne-linux)".
- **Buttons:** bevelled, drawn with `ImDrawList` (light top and left edges, dark bottom and right edges). This is the one decorative element. Everything else is flat.
- **Font:** the system's sans-serif font, found with fontconfig at run time, with ImGui's built-in font as the fallback. We don't copy a font into the repository: the OFL fonts in shadPS4's tree are not on the license allow-list (`docs/source-policy.md`).
- **Language:** English and German, chosen from the system locale, with English as the fallback. The main screen and the words every page shares live in one table per language (`src/launcher/text.cpp`). Each page keeps its own English and German strings in its own `page_*.cpp`, so pages added in parallel don't edit a shared table. The `Page` interface (draw, on_close) is documented in `src/launcher/page.hpp`. The scripted test input (`BB_LAUNCHER_SCRIPT`) is described in "Code layout and test input" below.

## States of the main screen

| State | Header | Status line | First button |
|---|---|---|---|
| No game folder yet | drop target | "No game found yet" | Set up game |
| Game folder fails its check | drop target | the check's message, in words | Set up game |
| Ready | title picture | "Version 1.09 · Controller connected" | Play |
| Ready, no controller | title picture | "No controller. Plug one in before you play." | Play |
| No usable Vulkan GPU | title picture | "Your graphics card can't run the game." | Help with problems |
| Last run ended with an error | title picture | "The game stopped unexpectedly." | Play (Help with problems is highlighted) |

The status line replaces a checklist page: when everything is fine it is one quiet line, and when something is wrong it says exactly one thing, the most important one. The order of checks is game folder, GPU, controller.

A missing controller doesn't block Play: the player may plug one in after pressing it. `bb-run` has no keyboard input for the game yet, so the hint is needed.

Buttons are never disabled. A button that can't do anything yet says why when pressed (Saves before setup: "Set up the game first.").

## First start

1. **Search.** The launcher looks for the game folder on its own: directories that hold `eboot.bin` and `sce_sys/param.sfo` with title ID CUSA03173, first in `game/` inside the launcher's folder, then under `$HOME` (up to four levels deep, without hidden directories) and under the removable-media mount points (`/run/media/$USER`, `/media`). The search is limited to two seconds and runs off the UI thread. One hit that passes the check is taken, and the status line shows the folder with a "Choose another folder" link. Several hits become a list to pick from.
2. **Drop or choose.** Otherwise the header becomes the drop target: "Drag your game folder here (the one with eboot.bin)". Set up game opens the folder dialog (`SDL_ShowOpenFolderDialog`).
3. **Check.** The setup check from #578 runs on the folder and its message goes to the status line in plain words (wrong folder, other game, base game without update 1.09, unknown version). The folder is written to the config file only when the check passes, relative to the launcher's folder when it lies inside it.

The search goes first because "where did I put the dump" is the most common place for a non-technical player to get stuck.

## Picture and sound

The settings read as two sentences with choices in them, not as a form:

> I play in **[a window | fullscreen]** at **[1280 × 720 | 1920 × 1080 | 2560 × 1440 | …]**, at **[30 | 60 | 120]** frames per second, with a **[normal | sharpened | extra sharp]** picture.
>
> I see it with **[the game's view | a wider view | a much wider view]**, **[textures as sharp as in the game | sharper distant textures | the sharpest distant textures]**, **[motion blur | no motion blur]** and **[colour fringes at the edges | no colour fringes]**.

The sentences say what the player gets, without technical terms. The second one holds the options of R11 (image quality). Its default choices are the game's own picture, and a choice the player left alone writes nothing, so the defaults leave the config file as it was. The mapping onto the config file from #577 (and so onto `bb-run`'s options):

| Choice | Config / `bb-run` |
|---|---|
| a window / fullscreen | `--fullscreen` (borderless, at the display's size) |
| resolution | `--window-size WxH`. The list holds the common 16:9 sizes up to the display's size, and on a 21:9 or 32:9 display (width at least twice, or three times, the height) the wide sizes of that shape that fit it (2560 × 1080 and 3440 × 1440, or 5120 × 1440). With fullscreen the sentence reads "at your screen's size" instead of a choice |
| a wide size (window), or fullscreen on a 21:9 or 32:9 display | also `--resolution WxH`, the config key `resolution`: the game renders at the wide size (#667). In fullscreen it is the smallest wide size of the display's shape that is at least as tall as the display, else the largest, so FSR only scales up. A 16:9 choice removes a wide `resolution` again; a 16:9 `resolution` set by hand stays |
| 30 / 60 / 120 | `--frame-rate` (the rates of `bb::common::frame_rates`) |
| normal | no FSR |
| sharpened | `--fsr`, RCAS at its default attenuation (0.25) |
| extra sharp | `--fsr --rcas-attenuation 0` |
| the game's / a wider / a much wider view | `--fov`: no key (43°), 52°, 60° (#666). Another value in the file shows as its degrees |
| textures as in the game / sharper / the sharpest | `--anisotropy`: no key (the game's filtering), 4, 16 (#658). Another level shows as its factor |
| motion blur / no motion blur | `motion_blur = true / false` (`--no-motion-blur`, #665) |
| colour fringes / no colour fringes | `chromatic_aberration = true / false` (`--no-chromatic-aberration`, #665) |

The strengths of motion blur and chromatic aberration (`--motion-blur-strength`, `--chromatic-aberration-strength`) have no choice; a strength in the file stays. Options that the shadow and draw-distance issues of R11 add get their place in the second sentence in their own issue. The two sentences need more than the page's height at a small display scale, so the page scrolls, and gamepad navigation scrolls it to the focused choice.

Sound gets choices once `bb-run` has any (output device, volume). Until then the page is called "Picture and sound" anyway, so the button list doesn't change later.

Changes are saved when the page closes. There is no Apply button.

## Saves

- The profiles (`--profile`), each with the time it was last played. Choose one, make a new one, rename one.
- "Open folder" opens the profile's save folder in the file manager (`SDL_OpenURL` with a `file://` URL), so players can make backups without knowing where in the folder their saves are.
- Deleting a profile is not offered. Players who really want to delete one can do it in the file manager.

## Help with problems

- The messages for the problems we know, in words, each with what to do: game folder moved or unreadable, wrong version, no Vulkan GPU or driver too old, game stopped unexpectedly.
- **Copy problem report:** copies to the clipboard the launcher and `bb-run` versions, distribution, kernel, GPU and driver, the last run's exit status and the last 200 lines of its log. Home-directory paths are replaced with `~` and the user name never appears. The player can paste the report into an issue.
- **Open log folder.**

The page lists the four problems with what to do and puts the ones that apply now first (the check of the game folder, the GPU probe, the last run's status). Play sends the output of `bb-run` to `user/logs/launcher-last-run.log` and writes how it ended to `launcher-last-run.status` next to it, because `bb-run` has no log file of its own when it stops early. The report takes the last 200 lines of that capture, then replaces the launcher's folder with `<folder>`, the home directory and any `/home/<name>` with `~` and every whole word equal to the user name with `<user>`. The launcher has no version number yet, so the report gives the commit it was built from; `bb-run` is built with it.

## Starting the game

- Play starts `bb-run` from the launcher's own directory (found via `/proc/self/exe`) as a separate process, with `--config <path>` and nothing else. The launcher holds no settings of its own: the config file is the only state.
- The launcher hides its window while the game runs and waits for the process to end. On exit code 0 it comes back to the main screen. On any other exit it comes back with the error state above.
- The launcher never writes into the game folder, and nothing outside `user/` in its own folder.

## Controller

Each part of the launcher can be used with the controller alone: ImGui's gamepad navigation (`ImGuiConfigFlags_NavEnableGamepad`) through its SDL3 backend. Focus starts on the first button. A (Cross) chooses, B (Circle) goes back. Mouse and keyboard work as well. This covers the couch and handheld PCs without a separate "big picture" mode.

## Technical choices

- **Renderer:** ImGui's SDL_Renderer backend (`imgui_impl_sdlrenderer3`), not Vulkan. The launcher has to start, and show the problem, when Vulkan is missing or broken. It probes Vulkan separately by opening the loader at run time and listing the physical devices.
- **Its own ImGui build:** `bb_imgui` is built with shadPS4's ImGui config, which changes `ImTextureID` and the index size. The launcher doesn't link the renderer, so it builds its own ImGui library with the default config, from the same fetched sources.
- **PNG decoding:** `stb_image.h` (dual-licensed, used under MIT), copied unchanged into `third_party/stb/` in a PR of its own. SDL3 itself only loads BMP.
- **Config file:** read and written through the library from #577, which `bb-run` also links. The launcher keeps no second copy of the format.

## Code layout and test input

The code is in `src/launcher/`: `app.cpp` (window and loop), `main_screen.cpp`, one `page_*.cpp` per page behind the `Page` interface of `page.hpp`, shared widgets in `widgets.hpp`, and the pure logic (states, crop, language, `bb-run` arguments, script parsing) in `logic.cpp`, which `tests/launcher` covers. The main screen and the common words have one string table per language (`text.cpp`). Each page keeps its own English and German strings in its own file, so pages can be built in parallel. Play looks for `bb-run` next to the launcher.

For the checks and the tests, `BB_LAUNCHER_SCRIPT` names a file with one step per line (`wait N`, `drop PATH`, `press accept|back|up|down|left|right`, `shot PATH`, `config PATH`, `waitgame SECONDS`, `quit`). The steps feed drops and gamepad presses, save the rendered frame as a PNG, point Play at another config file and end the game after a time. `waitgame` only counts as the step directly after the `press` that starts the game; any other step in between leaves the game running without a limit. With a script, the launcher acts as if a controller were connected. `BB_LAUNCHER_URL_LOG` and `BB_LAUNCHER_CLIPBOARD_LOG` name files that get the folder URLs of the Saves page and the report of the Help page instead of a file manager window and the clipboard.

## Not in scope

- PKG installs (roadmap, decision 2026-11-16).
- Installing into the system or a menu entry: the game is one portable folder (#728).
- Downloads and updates of any kind, including of the launcher itself.
- Keyboard input for the game. That belongs in `bb-run`, as an issue of its own.
- Themes, news, mod lists: they would make the launcher less simple.

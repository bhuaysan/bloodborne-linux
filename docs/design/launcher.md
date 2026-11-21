# Launcher (R9)

The launcher `bb-launcher` sets up the game and starts it. It's for players who have never opened a terminal: they choose the game folder of their own dump once, then press Play. The roadmap's R9 entry sets the scope: no PKG installs, nothing downloaded. This page records how the launcher looks and behaves, and why.

The guideline is: keep it simple, but don't look like every other launcher.

## One portable folder

The game is one folder that the player unpacks anywhere they can write. Nothing is installed, and there is no menu entry:

```
Bloodborne-Linux/
├── bb-launcher, bb-run, bloodborne-linux.svg
├── game/       optional: the dump put here is found without setup (README.txt says so)
├── licenses/   the GPL, and the notices of SDL3, FFmpeg and the code in third_party/
└── user/       config.toml, profiles/<name>/savedata/, cache/, logs/ (made on first start)
```

`tools/package-portable.sh` builds it with the `portable` preset (`cmake/portable.cmake`) and packs it as `Bloodborne-Linux-<commit>.tar.gz` (#731). SDL3, a minimal FFmpeg (only the decoders and demuxers the game's movies and sounds need, LGPL: the four movies in `dvdroot_ps4/movie/` are H.264 and AAC in MP4, and AJM decodes MP3) and libstdc++ are built in statically, so `bb-run` needs only glibc and the Vulkan driver, and `bb-launcher` also fontconfig. SDL's Wayland, X11 and sound backends (PipeWire, PulseAudio, ALSA) and libdecor stay dynamic: SDL opens what the system has, so one package fits desktops with different sound servers. The build checks that their headers are there, because SDL would otherwise leave a backend out with only a warning. Not yet covered: the oldest glibc the package runs on is the build machine's (a build on an older base is #737).

Everything the launcher and `bb-run` write goes to `user/` (`docs/design/m3-core-os.md`, "Runtime directories"). Deleting the folder removes the game with its settings, saves, caches and logs; nothing stays in `~/.config`, `~/.local/share`, `~/.cache` or `~/.local/state`, and later nothing in AppData on Windows. The dump stays where the player keeps it, and only its path is stored. A dump inside the folder is stored relative to it, so the whole folder can be moved or renamed, for example onto a USB stick. We don't copy the dump in: it is large, and players keep it where they like (#728).

A folder that can't be written (copied to `/usr/bin`, say) gets a line on the main screen that says so and asks to unpack the game into a folder of its own.

What we can't control: the folder dialog runs through xdg-desktop-portal, which may add the chosen folder to the desktop's "recently used" list (dragging the folder onto the window doesn't); a crash may leave a core dump with systemd-coredump; a graphics driver that ignores the cache variables writes its cache where it likes; and a file manager that "Open folder" starts fresh inherits the launcher's redirected `XDG_CACHE_HOME`, so its own caches may land in `user/cache/system` (one that is already running keeps its own).

## Layout and look: the game's system menu

A small window with a fixed size: the game's title picture at the top, fading into black, a thin line, then a short menu. The first entry is Play, and it is bigger than the others. The look follows the game's own system menu: near-black, serif type, entries without frames and a warm glow behind the chosen one. R9 drew a late-90s autorun menu with bevelled buttons; the maintainer found it amateurish and chose this look on 2026-11-21 (#742). The mock-ups also had ornaments in the game's style; plain lines won.

```
+--------------------------------+
|   title picture (pic1.png),    |
|   fading into black            |
|  ----------------------------  |
|                                |
|             Play               |   bigger, focused at start
|           Settings             |
|             Saves              |
|             Quit               |
|                                |
|  ----------------------------  |
|                     (x) Select |
+--------------------------------+
```

- **Size:** 480 × 600 logical pixels, not resizable, scaled with the display's content scale (`SDL_GetDisplayContentScale`). The pages behind the entries open in the same window, with their gold title between two lines. B, Escape or the Back hint leaves them. No second window.
- **Title picture:** `sce_sys/pic1.png` from the player's game folder (1920 × 1080 in the dump), cropped to the header's aspect ratio around its centre. Its lower half fades into the window's black. It is loaded at run time and never copied. We ship no game art, and every install still shows the game. Before the game folder is set up, the header shows the drop hint instead (see First start).
- **Window title:** the game's title from `sce_sys/param.sfo`, read at run time, plus "(bloodborne-linux)".
- **Entries:** centred text without a frame. The focused entry, or the one under the mouse, is light, with an amber glow behind it: an ellipse of vertex colours that fades out, drawn with `ImDrawList`.
- **Lines and hints:** one-pixel lines that fade out at both ends, under the header and above the hints for the controller's buttons at the bottom right (Cross Select, Circle Back). The button symbols are drawn from circles and lines, so no font needs their glyphs. The Back hint is also the mouse's Back button.
- **Colours:** near-black, bone-white text, gold titles, the amber glow; warnings amber, errors red. ImGui's own widgets on the pages (lists, choices, text fields) get the same colours from the theme in `widgets.cpp`. The choices of Picture and sound look like underlined words of their sentence.
- **Font:** a serif font of the system, found with fontconfig at run time: EB Garamond, Cormorant Garamond, else what fontconfig matches to "serif" (Noto Serif on Fedora), with ImGui's built-in font as the fallback. A Garamond comes closest to the game's menus. We don't copy a font into the repository: the OFL fonts in shadPS4's tree are not on the license allow-list (`docs/source-policy.md`).
- **Language:** English and German, chosen from the system locale, with English as the fallback. The main screen and the words every page shares live in one table per language (`src/launcher/text.cpp`). Each page keeps its own English and German strings in its own `page_*.cpp`, so pages added in parallel don't edit a shared table. The `Page` interface (draw, on_close) is documented in `src/launcher/page.hpp`. The scripted test input (`BB_LAUNCHER_SCRIPT`) is described in "Code layout and test input" below.

## States of the main screen

| State | Header | Under the line | First entry |
|---|---|---|---|
| No game folder yet | drop hint | "No game found yet" | Set up game |
| Game folder fails its check | drop hint | the check's message, in words | Set up game |
| Ready | title picture | nothing | Play |

The line of text under the header replaces a checklist page. Without a game it says exactly one thing, the most important one; with a game there is no text at all. The maintainer wanted the main screen to show nothing but the menu (2026-11-21), so the other problems come up as a prompt when they matter: a band over the dimmed window with a gold title, one line of text and the options side by side.

| When | Prompt | Options |
|---|---|---|
| Play, without a usable Vulkan GPU | "No usable graphics card" | OK |
| Play, without a controller | "No controller connected" | Play anyway, Cancel |
| The game ended with an error | "The game stopped unexpectedly." and where its output is | OK |
| A problem while a game is set up (a refused drop, say) | "Note" and the message | OK |

Play checks the GPU first: without one, a controller doesn't help. A missing controller doesn't block Play: the player may plug one in after pressing it. `bb-run` has no keyboard input for the game yet, so the prompt is needed. B or Escape picks the last option.

Entries are never disabled. An entry that can't do anything yet says why when pressed (Saves before setup: "Set up the game first.").

## First start

1. **Search.** The launcher looks for the game folder on its own: directories that hold `eboot.bin` and `sce_sys/param.sfo` with title ID CUSA03173, first in `game/` inside the launcher's folder, then under `$HOME` (up to four levels deep, without hidden directories) and under the removable-media mount points (`/run/media/$USER`, `/media`). The search is limited to two seconds and runs off the UI thread. One hit that passes the check is taken; Settings, Set up game shows it with "Choose another folder". Several hits become a list to pick from.
2. **Drop or choose.** Otherwise the header becomes the drop target: "Drag your game folder here (the one with eboot.bin)". Set up game opens the folder dialog (`SDL_ShowOpenFolderDialog`).
3. **Check.** The setup check from #578 runs on the folder and its message goes under the line on the main screen (or into a prompt while another game is set up) in plain words (wrong folder, other game, base game without update 1.09, unknown version). The folder is written to the config file only when the check passes, relative to the launcher's folder when it lies inside it.

The search goes first because "where did I put the dump" is the most common place for a non-technical player to get stuck.

## Settings

A menu like the main one, with three entries:

- **Set up game:** the folder in use and the game's version, and "Choose another folder", which opens the folder dialog. Without a game, how to drop the folder and "Choose folder". The last refused folder's message shows here too.
- **Picture and sound:** the page below.
- **Controller:** whether a controller is connected, with the names SDL reports, or how to connect one.

The folder and the controller moved here from the main screen's old status line, so the main screen shows only the menu.

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

Sound gets choices once `bb-run` has any (output device, volume). Until then the page is called "Picture and sound" anyway, so the menu doesn't change later.

Changes are saved when the page closes. There is no Apply button.

## Saves

- The profiles (`--profile`), each with the time it was last played. Choose one, make a new one, rename one.
- "Open folder" opens the profile's save folder in the file manager (`SDL_OpenURL` with a `file://` URL), so players can make backups without knowing where in the folder their saves are.
- Deleting a profile is not offered. Players who really want to delete one can do it in the file manager.

## Starting the game

- Play starts `bb-run` from the launcher's own directory (found via `/proc/self/exe`) as a separate process, with `--config <path>` and nothing else. The launcher holds no settings of its own: the config file is the only state.
- The launcher hides its window while the game runs and waits for the process to end. On exit code 0 it comes back to the main screen. On any other exit it comes back with the error state above.
- Play sends the output of `bb-run` to `user/logs/launcher-last-run.log` and writes how it ended to `launcher-last-run.status` next to it, because `bb-run` has no log file of its own when it stops early.
- The launcher never writes into the game folder, and nothing outside `user/` in its own folder.

## Controller

Each part of the launcher can be used with the controller alone: ImGui's gamepad navigation (`ImGuiConfigFlags_NavEnableGamepad`) through its SDL3 backend. Focus starts on the first entry. A (Cross) chooses, B (Circle) goes back. Mouse and keyboard work as well. This covers the couch and handheld PCs without a separate "big picture" mode.

## Technical choices

- **Renderer:** ImGui's SDL_Renderer backend (`imgui_impl_sdlrenderer3`), not Vulkan. The launcher has to start, and show the problem, when Vulkan is missing or broken. It probes Vulkan separately by opening the loader at run time and listing the physical devices.
- **Its own ImGui build:** `bb_imgui` is built with shadPS4's ImGui config, which changes `ImTextureID` and the index size. The launcher doesn't link the renderer, so it builds its own ImGui library with the default config, from the same fetched sources.
- **PNG decoding:** `stb_image.h` (dual-licensed, used under MIT), copied unchanged into `third_party/stb/` in a PR of its own. SDL3 itself only loads BMP.
- **Config file:** read and written through the library from #577, which `bb-run` also links. The launcher keeps no second copy of the format.

## Code layout and test input

The code is in `src/launcher/`: `app.cpp` (window and loop), `main_screen.cpp`, one `page_*.cpp` per page behind the `Page` interface of `page.hpp`, shared widgets in `widgets.hpp`, and the pure logic (states, crop, language, `bb-run` arguments, script parsing) in `logic.cpp`, which `tests/launcher` covers. The main screen and the common words have one string table per language (`text.cpp`). Each page keeps its own English and German strings in its own file, so pages can be built in parallel. Play looks for `bb-run` next to the launcher.

For the checks and the tests, `BB_LAUNCHER_SCRIPT` names a file with one step per line (`wait N`, `drop PATH`, `press accept|back|up|down|left|right`, `shot PATH`, `config PATH`, `waitgame SECONDS`, `nopad`, `quit`). The steps feed drops and gamepad presses, save the rendered frame as a PNG, point Play at another config file and end the game after a time. `waitgame` only counts as the step directly after the `press` that starts the game; any other step in between leaves the game running without a limit. With a script, the launcher acts as if a controller were connected until a `nopad` step, whatever is plugged into the machine, so the prompt for a missing controller can be tested. `BB_LAUNCHER_URL_LOG` names a file that gets the folder URLs of the Saves page instead of a file manager window.

## Not in scope

- PKG installs (roadmap, decision 2026-11-16).
- Installing into the system or a menu entry: the game is one portable folder (#728).
- Downloads and updates of any kind, including of the launcher itself.
- Keyboard input for the game. That belongs in `bb-run`, as an issue of its own.
- Themes, news, mod lists: they would make the launcher less simple.
- A help page with the known problems and a copyable problem report. R9 had one; the maintainer dropped it on 2026-11-21 (#741) to keep the launcher to its few entries. Problems show where they happen, and the run log stays in `user/logs`.

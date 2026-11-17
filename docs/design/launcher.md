# Launcher (R9)

The launcher `bb-launcher` sets up the game and starts it. It's for players who have never opened a terminal: they choose the game folder of their own dump once, then press Play. The roadmap's R9 entry sets the scope: no PKG installs, nothing downloaded. This page records how the launcher looks and behaves, and why.

The guideline is: keep it simple, but don't look like every other launcher.

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
- **Language:** English and German, chosen from the system locale, with English as the fallback. The main screen and the words every page shares live in one table per language (`src/launcher/text.cpp`). Each page keeps its own English and German strings in its own `page_*.cpp`, so pages added in parallel don't edit a shared table. The `Page` interface (draw, on_close) is documented in `src/launcher/page.hpp`. The scripted test input (`BB_LAUNCHER_SCRIPT`) comes with the window and is documented here when it lands.

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

1. **Search.** The launcher looks for the game folder on its own: directories that hold `eboot.bin` and `sce_sys/param.sfo` with title ID CUSA03173, under `$HOME` (up to four levels deep, without hidden directories) and under the removable-media mount points (`/run/media/$USER`, `/media`). The search is limited to two seconds and runs off the UI thread. One hit that passes the check is taken, and the status line shows the folder with a "Choose another folder" link. Several hits become a list to pick from.
2. **Drop or choose.** Otherwise the header becomes the drop target: "Drag your game folder here (the one with eboot.bin)". Set up game opens the folder dialog (`SDL_ShowOpenFolderDialog`).
3. **Check.** The setup check from #578 runs on the folder and its message goes to the status line in plain words (wrong folder, other game, base game without update 1.09, unknown version). The folder is written to the config file only when the check passes.

The search goes first because "where did I put the dump" is the most common place for a non-technical player to get stuck.

## Picture and sound

The settings read as a sentence with choices in it, not as a form:

> I play in **[a window | fullscreen]** at **[1280 × 720 | 1920 × 1080 | 2560 × 1440 | …]**, at **[30 | 60]** frames per second, with a **[normal | sharpened | extra sharp]** picture.

The sentence says what the player gets, without technical terms. The mapping onto the config file from #577 (and so onto `bb-run`'s options):

| Choice | Config / `bb-run` |
|---|---|
| a window / fullscreen | `--fullscreen` (borderless, at the display's size) |
| resolution | `--window-size WxH`. The list holds the common 16:9 sizes up to the display's size. With fullscreen the sentence reads "at your screen's size" instead of a choice |
| 30 / 60 | `--frame-rate` |
| normal | no FSR |
| sharpened | `--fsr`, RCAS at its default attenuation (0.25) |
| extra sharp | `--fsr --rcas-attenuation 0` |

Sound gets choices once `bb-run` has any (output device, volume). Until then the page is called "Picture and sound" anyway, so the button list doesn't change later.

Changes are saved when the page closes. There is no Apply button.

## Saves

- The profiles (`--profile`), each with the time it was last played. Choose one, make a new one, rename one.
- "Open folder" opens the profile's save folder in the file manager (`SDL_OpenURL` with a `file://` URL), so players can make backups without knowing where XDG directories are.
- Deleting a profile is not offered. Players who really want to delete one can do it in the file manager.

## Help with problems

- The messages for the problems we know, in words, each with what to do: game folder moved or unreadable, wrong version, no Vulkan GPU or driver too old, game stopped unexpectedly.
- **Copy problem report:** copies to the clipboard the launcher and `bb-run` versions, distribution, kernel, GPU and driver, the last run's exit status and the last 200 lines of its log. Home-directory paths are replaced with `~` and the user name never appears. The player can paste the report into an issue.
- **Open log folder.**

## Starting the game

- Play starts `bb-run` from the launcher's own directory (found via `/proc/self/exe`) as a separate process, with `--config <path>` and nothing else. The launcher holds no settings of its own: the config file is the only state.
- The launcher hides its window while the game runs and waits for the process to end. On exit code 0 it comes back to the main screen. On any other exit it comes back with the error state above.
- The launcher never writes into the game folder, and nothing outside the XDG directories.

## Controller

Each part of the launcher can be used with the controller alone: ImGui's gamepad navigation (`ImGuiConfigFlags_NavEnableGamepad`) through its SDL3 backend. Focus starts on the first button. A (Cross) chooses, B (Circle) goes back. Mouse and keyboard work as well. This covers the couch and handheld PCs without a separate "big picture" mode.

## Technical choices

- **Renderer:** ImGui's SDL_Renderer backend (`imgui_impl_sdlrenderer3`), not Vulkan. The launcher has to start, and show the problem, when Vulkan is missing or broken. It probes Vulkan separately by opening the loader at run time and listing the physical devices.
- **Its own ImGui build:** `bb_imgui` is built with shadPS4's ImGui config, which changes `ImTextureID` and the index size. The launcher doesn't link the renderer, so it builds its own ImGui library with the default config, from the same fetched sources.
- **PNG decoding:** `stb_image.h` (dual-licensed, used under MIT), copied unchanged into `third_party/stb/` in a PR of its own. SDL3 itself only loads BMP.
- **Config file:** read and written through the library from #577, which `bb-run` also links. The launcher keeps no second copy of the format.

## Not in scope

- PKG installs (roadmap, decision 2026-11-16).
- Downloads and updates of any kind, including of the launcher itself.
- Keyboard input for the game. That belongs in `bb-run`, as an issue of its own.
- Themes, news, mod lists: they would make the launcher less simple.

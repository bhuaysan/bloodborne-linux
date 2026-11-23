# bloodborne-linux

Bloodborne (PS4) running natively on Linux. No emulator, no CPU translation.

> [!IMPORTANT]
> This repository contains no game files, and never will. You need your own dump of the game.

## What it is

The PS4 has an x86-64 CPU, like your PC. So the game's own machine code can run directly on Linux. This project builds what is missing around it:

- **Loader.** Maps the game executable into memory and starts it.
- **System libraries.** The PS4's operating system functions, rebuilt on Linux.
- **Graphics.** Translates the PS4's GPU commands and shaders to Vulkan.

Where the game itself needs fixing, single functions are rewritten in C++. That is how the frame rate goes past 30. Everything else stays the game's own code.

Think of it as Wine plus DXVK, built for one game. The renderer, the shader translator and some libraries come from [shadPS4](https://github.com/shadps4-emu/shadPS4). Every source is credited in [docs/references.md](docs/references.md).

## Status

| | |
|---|---|
| Works | Title, character creation, intro, saving and loading, Central Yharnam with fights |
| Frame rate | 30 (the game's own), 60 and 120 FPS |
| Picture | Up to 3840 × 2160, 21:9 and 32:9, wider field of view, FSR 1 upscaling |
| Not checked yet | A full playthrough to an ending |

What's next is in the [roadmap](docs/roadmap.md).

## What you need

- Linux on x86-64
- A GPU with a Vulkan driver. We test on an AMD RX 7800 XT with RADV.
- A gamepad. Keyboard and mouse don't work in the game yet.
- Your own dump of Bloodborne **CUSA03173 with update 1.09**, as a folder that holds `eboot.bin`

## Play

Download from [Releases](https://github.com/bhuaysan/bloodborne-linux/releases):

- **`Bloodborne-Linux-….AppImage`** (recommended): one file. Make it executable (`chmod +x`) and start it. On the first start it asks for a data folder, which holds settings, saves, shader cache, logs and optionally your dump in `game/`.
- **`Bloodborne-Linux-….tar.gz`**: the same as a folder, for systems without FUSE. Unpack it anywhere you can write and start `bb-launcher` in it.

Check the download with `sha256sum -c --ignore-missing SHA256SUMS`. Then:

1. Drag your game folder onto the launcher window. Or put the dump into `game/` beforehand, then the launcher finds it on its own.
2. Press **Play**.

The launcher works with the gamepad, too. Cross (A) chooses, Circle (B) goes back.

**Settings → Picture and sound** sets window or fullscreen, monitor, graphics card, resolution, frame rate, VSync, sharpening, field of view, texture filtering, motion blur and colour fringes. The defaults look like the PS4. **Settings → Advanced** can shorten the loading screen after you die.

**Saves** keeps separate profiles, for example one for testing and one for your playthrough. **Open folder** shows a profile's saves, so you can back them up.

If the game stops unexpectedly, its output is in `user/logs/launcher-last-run.log`. Open an [issue](https://github.com/bhuaysan/bloodborne-linux/issues) and paste the end of it, after taking out your user name and folder paths.

## The folder

The tar.gz unpacks to this folder (the AppImage keeps the same `user/` and `game/` in its data folder):

```
Bloodborne-Linux/
├── bb-launcher    start this
├── bb-run         the game itself
├── lib/           bundled libraries
├── game/          optional: put your dump here
└── user/          settings, saves, shader cache, logs
```

Everything the game writes stays in `user/`. Delete the folder and it's all gone. Your dump is only read, never written.

You can move or rename the folder, for example onto a USB stick. A dump inside `game/` moves with it.

Played an older build? Its saves are in `~/.local/share/bloodborne-linux`. Copy them over once:

```bash
cp -r ~/.local/share/bloodborne-linux/profiles Bloodborne-Linux/user/
```

## Without the launcher

`bb-run` is the game itself. It reads the launcher's settings from `user/config.toml`, and its options override them.

```bash
./bb-run --frame-rate 60 --fullscreen
```

`./bb-run --help` lists every option.

## Supported distributions

Releases start on glibc 2.31 and newer. The package brings its own SDL, FFmpeg and C++ library and takes the Vulkan driver, the display libraries and the sound libraries from the system. Each line below played the route from a new game to first control in the clinic, with sound, on an AMD GPU (RADV) under KDE Plasma on Wayland (`docs/test-rig.md`, "Distributions").

| Distribution | Oldest version | What to install | Known limits |
|---|---|---|---|
| Fedora | 44 | nothing on a desktop | the reference machine |
| Ubuntu | 22.04 | `mesa-vulkan-drivers` (AMD and Intel) | none known |
| Debian | 12 | `mesa-vulkan-drivers` (AMD and Intel); on Debian 13 from `trixie-backports` | Debian 13's own Mesa 25.0.7 hangs the GPU in the intro after the character editor; the backports driver doesn't (#793) |
| Arch Linux | rolling | `vulkan-radeon` (AMD) or `vulkan-intel` | none known |
| Bazzite | stable image | nothing | none known |

A system without a desktop (a server or container image) also needs the libraries a desktop brings: on Ubuntu and Debian `libxkbcommon0 libwayland-cursor0 libwayland-egl1 libxext6 libasound2t64 libpulse0 libpipewire-0.3-0t64 pipewire-bin libfontconfig1` (on Ubuntu 22.04 and Debian 12 `libasound2` and `libpipewire-0.3-0`), on Arch `libxkbcommon libxext alsa-lib libpulse pipewire fontconfig`. Without the display libraries the window does not open; without the sound ones the game plays silently. NVIDIA's driver and Intel GPUs are not checked yet. The game needs fewer than 2000 memory mappings, so the older `vm.max_map_count` default of 65530 is enough.

If the game hitches for one frame (50 to 100 ms) about once a minute, look for a program that reads the memory use of every process, such as a system monitor that shows PSS or shared memory, or a desktop app that polls `/proc/*/smaps_rollup`. While the kernel collects that figure for the game's 4 GiB of mapped memory, the game's write tracking has to wait. Closing that program removes the hitch (#792).

## Known limits

- At 60 FPS and above, a few small differences from 30 remain. They are listed in [docs/design/frame-rate.md](docs/design/frame-rate.md).
- 120 FPS holds only where the GPU keeps up. Faster frames are still work in progress.
- No PKG installs. Bring the game as a folder.

## Build it yourself

Install the build tools. On Fedora:

```bash
sudo dnf install git cmake ninja-build gcc-c++ python3 spirv-headers-devel spirv-tools vulkan-headers vulkan-loader-devel glslang SDL3-devel ffmpeg-free-devel gtest-devel libasan libubsan
```

Install lines for Ubuntu 24.04, Debian 13, Arch and Bazzite are in [docs/distributions.md](docs/distributions.md). You need CMake 3.27 or newer. The first build downloads Zydis and Dear ImGui.

Build the same folder as a release:

```bash
tools/package-portable.sh
```

Or build for development, with tests and sanitizers:

```bash
cmake --preset debug
```

```bash
cmake --build --preset debug
```

```bash
ctest --preset debug
```

| Preset | Use |
|---|---|
| `debug` | everyday work, AddressSanitizer and UBSan |
| `relwithdebinfo` | optimized and debuggable, for profiling |
| `release` | what releases are built from |

Builds in `build/` sit inside the source tree, and the game refuses to write there. To start them by hand, point `BB_USER_DIR` at a folder outside the checkout. Tests that need the game skip unless `BB_GAME_DIR` points at your dump.

## Contributing

Before your first commit, turn on the repository's git hooks:

```bash
git config core.hooksPath tools/githooks
```

Read these before you change code:

- [Architecture](docs/architecture.md): how the pieces fit
- [Source and license policy](docs/source-policy.md): what code we may use, and what never
- [AGENTS.md](AGENTS.md): the working rules, reviews and merge gate
- [Releases](docs/releasing.md): versions and how a release is made

## License

GPL-2.0-or-later, see [LICENSE](LICENSE). Code from other projects keeps its own license and notices. Code written before 2026-11-06 was published under MIT. The license does not cover the game.

Bloodborne is a trademark of Sony Interactive Entertainment. This project is not affiliated with Sony Interactive Entertainment or FromSoftware.

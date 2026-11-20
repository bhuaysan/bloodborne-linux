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

## Install

There are no prebuilt packages yet, so you build it yourself. It takes a few minutes.

**1. Install the build tools.** On Fedora:

```bash
sudo dnf install git cmake ninja-build gcc-c++ python3 spirv-headers-devel spirv-tools vulkan-headers vulkan-loader-devel glslang SDL3-devel ffmpeg-free-devel
```

Other distributions need the same packages under their own names. You need CMake 3.27 or newer.

**2. Get the source.**

```bash
git clone https://github.com/bhuaysan/bloodborne-linux.git
```

```bash
cd bloodborne-linux
```

**3. Build and install.** The first build downloads Zydis and Dear ImGui.

```bash
cmake --preset release -DBB_BUILD_TESTS=OFF
```

```bash
cmake --build --preset release
```

```bash
cmake --install build/release --prefix ~/.local
```

## Play

1. Open **Bloodborne on Linux** from your application menu. If it's not there yet, log out and back in, or run `~/.local/bin/bb-launcher`.
2. The launcher looks for your game folder on its own. If it finds none, press **Set up game** and pick the folder, or drag it onto the window.
3. Press **Play**.

The launcher works with the gamepad, too. Cross (A) chooses, Circle (B) goes back.

**Picture and sound** sets window or fullscreen, resolution, frame rate, sharpening, field of view, texture filtering, motion blur and colour fringes. The defaults look like the PS4.

**Saves** keeps separate profiles, for example one for testing and one for your playthrough. **Open folder** shows where a profile's saves are, so you can back them up.

**Help with problems** explains the errors we know and copies a problem report. Paste it into an [issue](https://github.com/bhuaysan/bloodborne-linux/issues) if something breaks. The report replaces your home folder and user name.

## Where your files go

The game folder is only read, never written.

| What | Where |
|---|---|
| Settings | `~/.config/bloodborne-linux/config.toml` |
| Saves | `~/.local/share/bloodborne-linux/profiles/<profile>/` |
| Shader cache | `~/.cache/bloodborne-linux/` |
| Logs | `~/.local/state/bloodborne-linux/logs/` |

These follow the `XDG_*_HOME` variables if you set them.

## Without the launcher

`bb-run` is the game itself. It reads the same config file, and its options override it.

```bash
bb-run --setup /path/to/CUSA03173
```

```bash
bb-run --frame-rate 60 --fullscreen
```

`bb-run --help` lists every option.

## Known limits

- At 60 FPS and above, a few small differences from 30 remain. They are listed in [docs/design/frame-rate.md](docs/design/frame-rate.md).
- 120 FPS holds only where the GPU keeps up. Faster frames are still work in progress.
- No PKG installs. Bring the game as a folder.

## For developers

Build with tests and sanitizers:

```bash
cmake --preset debug
```

```bash
cmake --build --preset debug
```

```bash
ctest --preset debug
```

The tests also need `gtest-devel`, `libasan` and `libubsan` (Fedora names). Tests that need the game skip unless `BB_GAME_DIR` points at your dump.

| Preset | Use |
|---|---|
| `debug` | everyday work, AddressSanitizer and UBSan |
| `relwithdebinfo` | optimized and debuggable, for profiling |
| `release` | what players build |

Before your first commit, turn on the repository's git hooks:

```bash
git config core.hooksPath tools/githooks
```

Read these before you change code:

- [Architecture](docs/architecture.md): how the pieces fit
- [Source and license policy](docs/source-policy.md): what code we may use, and what never
- [AGENTS.md](AGENTS.md): the working rules, reviews and merge gate

## License

GPL-2.0-or-later, see [LICENSE](LICENSE). Code from other projects keeps its own license and notices. Code written before 2026-11-06 was published under MIT. The license does not cover the game.

Bloodborne is a trademark of Sony Interactive Entertainment. This project is not affiliated with Sony Interactive Entertainment or FromSoftware.

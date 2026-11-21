# Building on other distributions

The runtime builds from source on Fedora 44 (the reference machine), Ubuntu 24.04, Debian 13 and
Arch Linux, with each distribution's own packages. Where a distribution lacks something or ships it
too old, CMake fetches a pinned copy and says so in the configure output.

| Distribution | Compiler used | Fetched by CMake |
|---|---|---|
| Fedora 44 | GCC 16 | nothing |
| Ubuntu 24.04 | GCC 14 (`g++-14`; the default GCC 13 can't build the shader translator) | SDL3, Vulkan-Headers |
| Debian 13 | GCC 14 | SDL3, Vulkan-Headers |
| Arch Linux | GCC 16 | nothing |

Install the build dependencies, then build as in the README. The lists are the files in
`tools/distro-packages/`, which `tools/distro-build.sh` installs in a fresh container; a test keeps
the lines below equal to them.

## Fedora

```sh
sudo dnf install git cmake ninja-build gcc-c++ gtest-devel python3 spirv-headers-devel spirv-tools vulkan-headers vulkan-loader-devel glslang mesa-vulkan-drivers llvm SDL3-devel ffmpeg-free-devel fontconfig-devel
```

## Ubuntu 24.04

```sh
sudo apt install git ca-certificates cmake ninja-build g++-14 pkg-config python3 libgtest-dev spirv-headers spirv-tools libvulkan-dev glslang-tools mesa-vulkan-drivers llvm libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libfontconfig-dev libwayland-dev wayland-protocols libxkbcommon-dev libdecor-0-dev libx11-dev libxext-dev libxcursor-dev libxi-dev libxfixes-dev libxrandr-dev libxtst-dev libpipewire-0.3-dev libpulse-dev libasound2-dev libegl-dev libdrm-dev libgbm-dev libudev-dev
export CC=gcc-14 CXX=g++-14   # before the first cmake command
```

## Debian 13

```sh
sudo apt install git ca-certificates cmake ninja-build g++ pkg-config python3 libgtest-dev spirv-headers spirv-tools libvulkan-dev glslang-tools mesa-vulkan-drivers llvm libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libfontconfig-dev libwayland-dev wayland-protocols libxkbcommon-dev libdecor-0-dev libx11-dev libxext-dev libxcursor-dev libxi-dev libxfixes-dev libxrandr-dev libxtst-dev libpipewire-0.3-dev libpulse-dev libasound2-dev libegl-dev libdrm-dev libgbm-dev libudev-dev
```

## Arch Linux

```sh
sudo pacman -S --needed git cmake ninja gcc pkgconf python gtest spirv-headers spirv-tools vulkan-headers vulkan-icd-loader glslang vulkan-swrast llvm sdl3 ffmpeg fontconfig ttf-dejavu
```

## Bazzite and other immutable systems

The system image has no compilers and no `-devel` packages. Build inside a Fedora container, which
shares your home folder, and run the game from inside it:

```sh
distrobox create --name bb-build --image registry.fedoraproject.org/fedora:44
distrobox enter bb-build
# inside: the Fedora line above, then the build from the README
```

## What gets fetched

- **Vulkan-Headers 1.4.341 and SPIRV-Headers (the commit of Fedora 44's package)**, when the system's
  copies lack what the code uses (`cmake/headers.cmake`; Ubuntu 24.04 and Debian 13). The configure
  output names the version found. `-DBB_FETCH_HEADERS=always` forces the fetched copies, `never`
  forbids them.
- **SDL3 3.4.20**, built statically from its release tarball, when no SDL3 package is found
  (`cmake/sdl3.cmake`; Ubuntu 24.04 and Debian 13). The `-dev` packages in the Ubuntu and Debian lines
  from `libwayland-dev` down are the headers its backends are built from.

All downloads are pinned by SHA-256 in the CMake files and listed in `docs/references.md`.

## Checking a distribution locally

```sh
tools/distro-build.sh ubuntu      # or fedora, debian, arch
```

Needs podman and a network. It installs the packages in a fresh official container, configures the
`release` preset with warnings as errors, builds, and runs the unit tests (tests that need the game
skip). It prints the compiler and the build time. CI runs it for Ubuntu, Debian and Arch on every
push to `main` (`.github/workflows/ci.yml`, job `distros`); Fedora is the main CI build.

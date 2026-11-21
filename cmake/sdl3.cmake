# SDL3 built from its release source and linked statically: the portable package always does
# (cmake/portable.cmake), and a distribution without an SDL3 package (Ubuntu 24.04) gets it from
# the top-level CMakeLists.txt (docs/distributions.md).

include_guard(GLOBAL)
include(FetchContent)
# SDL3 at the version this was developed against (zlib license). OVERRIDE_FIND_PACKAGE makes the
# find_package(SDL3 CONFIG) calls in src/ take this build instead of the system's. The backends
# (Wayland, X11, PipeWire, PulseAudio, ALSA) stay dynamic: SDL opens what the system has.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
# Backends SDL would otherwise leave out with only a warning: Wayland (with libdecor, which draws
# the window decorations on GNOME) and X11 for the window; PipeWire, PulseAudio and ALSA for the
# sound, whichever the system has. All are opened at run time; only their headers are needed here.
find_package(PkgConfig REQUIRED)
set(BB_SDL_MISSING)
foreach(module wayland-client wayland-egl wayland-cursor wayland-scanner xkbcommon libdecor-0
        x11 xext xcursor xi xfixes xrandr xtst libpipewire-0.3 libpulse alsa)
    string(MAKE_C_IDENTIFIER "BB_SDL_${module}" var)
    pkg_check_modules(${var} QUIET ${module})
    if(NOT ${var}_FOUND)
        list(APPEND BB_SDL_MISSING ${module})
    endif()
endforeach()
if(BB_SDL_MISSING)
    string(JOIN " " BB_SDL_MISSING ${BB_SDL_MISSING})
    message(FATAL_ERROR "Building SDL3 from source needs these headers for its backends: "
        "${BB_SDL_MISSING} (Fedora: wayland-devel libxkbcommon-devel "
        "libdecor-devel libX11-devel libXext-devel libXcursor-devel libXi-devel libXfixes-devel libXtst-devel "
        "libXrandr-devel pipewire-devel pulseaudio-libs-devel alsa-lib-devel; Debian and Ubuntu: "
        "libwayland-dev libxkbcommon-dev libdecor-0-dev libx11-dev libxext-dev "
        "libxcursor-dev libxi-dev libxfixes-dev libxrandr-dev libxtst-dev libpipewire-0.3-dev libpulse-dev "
        "libasound2-dev)")
endif()
# Parts the game and the launcher don't use, so their headers aren't needed to build.
foreach(part X11_XSCRNSAVER JACK SNDIO KMSDRM CAMERA)
    set(SDL_${part} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
    URL_HASH SHA256=f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SYSTEM EXCLUDE_FROM_ALL
    OVERRIDE_FIND_PACKAGE)
FetchContent_MakeAvailable(SDL3)
if(NOT TARGET SDL3::SDL3)
    add_library(SDL3::SDL3 ALIAS SDL3-static)
endif()

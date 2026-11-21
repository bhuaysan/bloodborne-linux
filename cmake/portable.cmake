# The portable package (BB_PORTABLE, the `portable` preset; docs/design/launcher.md, "One portable
# folder"): SDL3 and a minimal FFmpeg are built from their release sources and linked statically,
# and so are libstdc++ and libgcc. The package then needs only glibc, the Vulkan loader and driver,
# fontconfig and the display and audio libraries that SDL opens at run time.

include(FetchContent)
include(ExternalProject)

# SDL3 at the version this was developed against (zlib license). OVERRIDE_FIND_PACKAGE makes the
# find_package(SDL3 CONFIG) calls in src/ take this build instead of the system's. The backends
# (Wayland, X11, PipeWire, PulseAudio, ALSA) stay dynamic: SDL opens what the system has.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
# Backends SDL would otherwise leave out with only a warning: libdecor draws the window decorations
# on GNOME's Wayland; PipeWire, PulseAudio and ALSA play the sound, whichever the system has. All
# are opened at run time; only their headers are needed here.
find_package(PkgConfig REQUIRED)
set(BB_SDL_MISSING)
foreach(module libdecor-0 libpipewire-0.3 libpulse alsa)
    string(MAKE_C_IDENTIFIER "BB_SDL_${module}" var)
    pkg_check_modules(${var} QUIET ${module})
    if(NOT ${var}_FOUND)
        list(APPEND BB_SDL_MISSING ${module})
    endif()
endforeach()
if(BB_SDL_MISSING)
    string(JOIN " " BB_SDL_MISSING ${BB_SDL_MISSING})
    message(FATAL_ERROR "The portable package needs these headers for SDL's backends: "
        "${BB_SDL_MISSING} (Fedora: libdecor-devel pipewire-devel pulseaudio-libs-devel "
        "alsa-lib-devel; Debian and Ubuntu: libdecor-0-dev libpipewire-0.3-dev libpulse-dev "
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

# FFmpeg with only what the game's movies and sounds need (src/shadps4/core/libraries/avplayer and
# ajm: H.264, HEVC and AAC in MP4, MP3), LGPL-2.1-or-later: no --enable-gpl, no non-free parts,
# nothing autodetected. With nasm the decoders get their x86 assembly; without it they are slower.
find_program(BB_NASM nasm)
set(BB_FFMPEG_ASM --disable-x86asm)
if(BB_NASM)
    set(BB_FFMPEG_ASM --x86asmexe=${BB_NASM})
else()
    message(WARNING "nasm not found: FFmpeg is built without its x86 assembly (slower movies)")
endif()
set(BB_FFMPEG_PREFIX ${CMAKE_BINARY_DIR}/ffmpeg)
set(BB_FFMPEG_LIBS avformat avcodec swscale swresample avutil)
set(BB_FFMPEG_ARCHIVES)
foreach(lib IN LISTS BB_FFMPEG_LIBS)
    list(APPEND BB_FFMPEG_ARCHIVES ${BB_FFMPEG_PREFIX}/lib/lib${lib}.a)
endforeach()
ExternalProject_Add(bb_ffmpeg_build
    URL https://ffmpeg.org/releases/ffmpeg-8.1.3.tar.xz
    URL_HASH SHA256=7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    PREFIX ${CMAKE_BINARY_DIR}/ffmpeg-build
    INSTALL_DIR ${BB_FFMPEG_PREFIX}
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --enable-static
        --disable-shared --enable-pic --disable-programs --disable-doc --disable-network
        --disable-autodetect --disable-everything ${BB_FFMPEG_ASM}
        --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-swresample
        --enable-decoder=h264,hevc,aac,aac_fixed,mp3,mp3float
        --enable-parser=h264,hevc,aac,mpegaudio
        --enable-demuxer=mov,mp3,aac
        --enable-protocol=file
    BUILD_COMMAND make -j
    INSTALL_COMMAND make install
    BUILD_BYPRODUCTS ${BB_FFMPEG_ARCHIVES}
    USES_TERMINAL_BUILD ON)
file(MAKE_DIRECTORY ${BB_FFMPEG_PREFIX}/include)
# Linked in dependency order. add_dependencies on an INTERFACE library holds for its consumers.
add_library(bb_ffmpeg INTERFACE)
target_include_directories(bb_ffmpeg SYSTEM INTERFACE ${BB_FFMPEG_PREFIX}/include)
target_link_libraries(bb_ffmpeg INTERFACE ${BB_FFMPEG_ARCHIVES} m pthread)
add_dependencies(bb_ffmpeg bb_ffmpeg_build)

# No libstdc++ or libgcc of the build machine in the package.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LINK_OPTIONS -static-libstdc++ -static-libgcc)
check_cxx_source_compiles("#include <string>\nint main() { return std::string(\"x\").size() - 1; }"
    BB_HAVE_STATIC_LIBSTDCXX)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(NOT BB_HAVE_STATIC_LIBSTDCXX)
    message(FATAL_ERROR "The portable package links libstdc++ statically: install its static "
        "library (Fedora: libstdc++-static, Debian and Ubuntu: part of libstdc++-<n>-dev)")
endif()
add_link_options(-static-libstdc++ -static-libgcc)

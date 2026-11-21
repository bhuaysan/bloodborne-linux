bloodborne-linux is free software under the GNU General Public License, version 2 or later
(bloodborne-linux-GPL-2.0). Its complete source code, including the scripts that build this
package, is at https://github.com/bhuaysan/bloodborne-linux ; the version is in the archive's
name and printed by "bb-run --version". A version vX.Y.Z is the tag of that name; one like
v0.2.0-3-gabc1234 is the commit abc1234.

Built into the programs, each under its own license in this folder:

- FFmpeg 8.1.3 (libavcodec, libavformat, libavutil, libswscale, libswresample), LGPL 2.1 or
  later (FFmpeg-LGPL-2.1.txt), built unchanged from https://ffmpeg.org/releases/ffmpeg-8.1.3.tar.xz
  with the options in cmake/portable.cmake of the source above. To use another FFmpeg, build
  the programs from that source against it.
- SDL 3.4.20, zlib license (SDL3-zlib.txt), from
  https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
- The code in third_party/, each folder with its license (shadPS4: GPL 2.0 or later, with MIT
  and BSD-3-Clause parts).
- The GNU C++ library and the GCC runtime, under the GCC Runtime Library Exception.

The AppImage (not the tar.gz) also contains the AppImage runtime, an unchanged binary of
https://github.com/AppImage/type2-runtime/releases/tag/20251108 (MIT, runtime-x86_64), built
from https://github.com/AppImage/type2-runtime/tree/dd6cebedcbddde9c82f89b011e8e1d40b6e43868
It has libfuse 3.15.0 (LGPL 2.1, source: https://github.com/libfuse/libfuse/tree/fuse-3.15.0),
squashfuse (BSD-2-Clause), musl (MIT), zstd, zlib and mimalloc linked statically; their license
texts are in licenses/appimage-runtime/ inside the image. The runtime is the start of the file and
the squashfs image with the programs follows it, so the runtime can be replaced: the image starts
at the offset printed by "<file>.AppImage --appimage-offset" (write another runtime, then the
bytes from that offset on). "--appimage-extract" unpacks the image to look at it.

The game itself is not part of this package: you need your own dump of Bloodborne (CUSA03173)
with update 1.09.

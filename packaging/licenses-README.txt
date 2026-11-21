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

The game itself is not part of this package: you need your own dump of Bloodborne (CUSA03173)
with update 1.09.

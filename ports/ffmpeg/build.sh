#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
mode=${1:-mingw64}
# Full official n8.1 checkout (git clone --depth 1 --branch n8.1 https://github.com/FFmpeg/FFmpeg.git work/deps/ffmpeg-n8.1).
src=${FFMPEG_SOURCE:-$repo/work/deps/ffmpeg-n8.1}
if command -v cygpath >/dev/null; then src=$(cygpath -u "$src"); fi
out=$repo/work/bink_native/ffmpeg-$mode
pin=9047fa1b084f76b1b4d065af2d743df1b40dfb56
test "$(git -C "$src" rev-parse HEAD)" = "$pin" || { echo "Expected FFmpeg n8.1 $pin" >&2; exit 2; }
mkdir -p "$out/build" "$out/install"
common=(--prefix="$out/install" --disable-everything --disable-autodetect
 --disable-programs --disable-doc --disable-network --disable-avdevice
 --disable-avfilter --disable-swresample --disable-pthreads --disable-w32threads
 --disable-os2threads --enable-swscale --enable-avformat --enable-avcodec
 --enable-avutil --enable-demuxer=bink
 --enable-decoder=bink,binkaudio_dct,binkaudio_rdft --enable-protocol=file
 --disable-debug)
case "$mode" in
 mingw64)
  export PATH=/mingw64/bin:/usr/bin:$PATH
  # Upstream makefiles use POSIX paths/shell commands: use MSYS GNU make.
  make_cmd=${MAKE:-make}
  platform=(--target-os=mingw32 --arch=x86_64 --cc=gcc --cxx=g++ --disable-x86asm --enable-shared --disable-static)
  ;;
 android-arm64|android-x86_64)
  : "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to NDK 28.2.13676358}"
  if command -v cygpath >/dev/null; then ANDROID_NDK_HOME=$(cygpath -u "$ANDROID_NDK_HOME"); fi
  host=${NDK_HOST_TAG:-windows-x86_64}
  tc=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$host/bin
  # Host tools (configure probes, generators) use MSYS2 MinGW gcc.
  export PATH="$tc:$PATH:/mingw64/bin"
  make_cmd=${MAKE:-make}
  # Direct clang avoids executing Windows .cmd compiler wrappers from MSYS.
  # -j1 by default below; this dependency build never compiles lifted game code.
  # x86_64 is the emulator build: no asm (no nasm; avoids PIC text relocations).
  if [ "$mode" = android-arm64 ]; then arch=aarch64; asm=(); else arch=x86_64; asm=(--disable-asm); fi
  triple=$arch-linux-android29
  platform=(--target-os=android --arch=$arch --enable-cross-compile
    --cc=clang --cxx=clang++ --extra-cflags=--target=$triple
    --extra-ldflags="--target=$triple -Wl,-z,max-page-size=16384" "${asm[@]}"
    --ar=llvm-ar --nm=llvm-nm --ranlib=llvm-ranlib --strip=llvm-strip
    --disable-symver --enable-pic --enable-static --disable-shared)
  ;;
 *) echo "usage: $0 [mingw64|android-arm64|android-x86_64]" >&2;exit 2;;
esac
command -v "$make_cmd" >/dev/null || {
 echo "MSYS/POSIX GNU make is required; see build_mingw_probe.py for the native test fallback" >&2
 exit 2
}
cd "$out/build"
bash "$src/configure" "${common[@]}" "${platform[@]}"
"$make_cmd" -j"${JOBS:-1}" SHELL="$(command -v sh)"
"$make_cmd" install SHELL="$(command -v sh)"

#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
mode=${1:-mingw64}
src=${FFMPEG_SOURCE:-$repo/work/bink_native/ffmpeg-src}
out=$repo/work/bink_native/ffmpeg-$mode
mkdir -p "$out/build" "$out/install"
common=(--prefix="$out/install" --disable-everything --disable-autodetect
 --disable-programs --disable-doc --disable-network --disable-avdevice
 --disable-avfilter --disable-swresample --disable-pthreads --disable-w32threads
 --disable-os2threads --enable-swscale --enable-avformat --enable-avcodec
 --enable-avutil --enable-demuxer=bink
 --enable-decoder=bink,binkaudio_dct,binkaudio_rdft --enable-protocol=file
 --enable-shared --disable-static --disable-debug)
case "$mode" in
 mingw64)
  export PATH=/mingw64/bin:/usr/bin:$PATH
  # Upstream makefiles use POSIX paths/shell commands: use MSYS GNU make.
  make_cmd=${MAKE:-make}
  platform=(--target-os=mingw32 --arch=x86_64 --cc=gcc --cxx=g++ --disable-x86asm)
  ;;
 android-arm64)
  : "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to NDK 28.2.13676358}"
  host=${NDK_HOST_TAG:-windows-x86_64}
  tc=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$host/bin
  export PATH="$tc:$PATH"
  make_cmd=${MAKE:-make}
  platform=(--extra-ldflags=-Wl,-z,max-page-size=16384 --target-os=android --arch=aarch64 --enable-cross-compile --cc=aarch64-linux-android26-clang --cxx=aarch64-linux-android26-clang++ --ar=llvm-ar --nm=llvm-nm --ranlib=llvm-ranlib --strip=llvm-strip --disable-symver --enable-pic)
  ;;
 *) echo "usage: $0 [mingw64|android-arm64]" >&2;exit 2;;
esac
command -v "$make_cmd" >/dev/null || {
 echo "MSYS/POSIX GNU make is required; see build_mingw_probe.py for the native test fallback" >&2
 exit 2
}
cd "$out/build"
bash "$src/configure" "${common[@]}" "${platform[@]}"
"$make_cmd" -j"${JOBS:-8}" SHELL="$(command -v sh)"
"$make_cmd" install SHELL="$(command -v sh)"

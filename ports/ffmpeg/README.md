# AC1 native Bink prototype

The provider is integrated into the Windows host runtime behind
`AC1_NATIVE_BINK` (default ON on Windows, OFF on Android). The adaptation of recomp-kit `dx/bink.cpp` retains its MIT notice in
`src/platform/ac1_bink_native.cpp`. FFmpeg remains a separate LGPL dependency.
All executable-derived disassembly, media, decoder libraries, executables and
test logs are private under ignored `work/bink_native/`.

## Observed AC1 ABI

The executable SHA-256 is
`2fa8d50c60398cc249c6d908de46b15f48e445dd0c605b5956e94fa2f445a713`.
The DLL matches `config/binkw32.json`:
`603d0a96d1f27422f88d78b20ca525975ebe5e0d7667e62b98a3db80766702b4`.
Addresses below are preferred x86 virtual addresses. Capstone/pefile were run
with the read-only reference venv's `python -B`. These are static observations,
not a claim that AC1 has executed this provider.

All 14 imports are in the IAT at `0x01a47f98..0x01a47fcc`. There are 20 actual
call instructions in the player implementation, including register-indirect
calls after loading an IAT slot. The linker also emits 14 unused six-byte import
thunks at `0x0114b5d8..0x0114b626`; the whole-image scan found no direct branch or
absolute pointer references to those thunks.

| Import (Bink prefix omitted) | Actual call sites | Arguments / use |
| --- | --- | --- |
| Wait@4 | `009e9157` | Player's BINK handle, return gates decode |
| SetSoundTrack@8 | `009e92b1` | Count 1, ID list at `01a2558c` |
| ShouldSkip@4 | `009e91a3`, `009e91db` | Handle; catch-up loop still decodes skipped frames |
| NextFrame@4 | `009e91ba`, `009e91f2` | Handle; after skipped decode or displayed copy |
| SetSoundSystem@8 | `009e92c3` | Imported OpenDirectSound function address, result of `007e2bb0` |
| DoFrame@4 | `009e9182`, `009e91c0` | Handle; first decode and skip-loop decode |
| Open@8 | `009e92fc` | Filename string, **`0x4000` (BINKSNDTRACK)** |
| SetVolume@12 | `009e8ba2`, `009e8bbd` | Handle, track ID, **`0x8000`** (unity) on close |
| Pause@8 | `009e8b56`, `009e8b66` | Handle, 1 / 0 |
| Close@4 | `009e8bdb` | Handle, then player's handle is zeroed |
| SetMemory@8 | `009e9144` | Guest allocator `009e8d90`, guest free `009e90d0` |
| GetFrameBuffersInfo@8 | `009e8e44`, `009e8eb7`, `009e9370` | Handle, player+`0x98` |
| CopyToBuffer@28 | `009e8f20` | Handle, locked pBits, locked Pitch, texture height, x=0, y=0, **`0x80000005`** |
| OpenDirectSound@4 | No direct game call | Address loaded at `009e92bd`, passed to SetSoundSystem |

The selected-ID global is in zero-initialized storage. No direct writes were
found in the executable, so the observed initial selection is **ID 0**, not four
simultaneous audio tracks. At close, a branch on `019310b0` resets either that
single ID or IDs `{0,1,2,3}` from `01934020` to volume 32768. This four-ID table
is **not** passed to SetSoundTrack. Selection remains an argument to the provider
because runtime settings or aliases cannot be ruled out by this static scan.

The handle lives at player+`0x78`. Observed **direct BINK record reads**:

| Offset | Meaning | Read sites |
| --- | --- | --- |
| `08` | Frames | `009e8c9b/009e8ca0`, `009e9190`, `009e91ce` |
| `0c` | FrameNum, 1-based | `009e8c43/009e8c48`, `009e918d`, `009e91cb` |
| `14` | FrameRate numerator | `009e8bf4/009e8bf7` |
| `18` | FrameRateDiv denominator | `009e8c04/009e8c07` |

No direct game reads of Width, Height, LastFrameNum or Paused were found in the
player. The provider nevertheless writes the correct prefix: Width `00`,
Height `04`, Frames `08`, FrameNum `0c`, LastFrameNum `10`, FrameRate `14`,
FrameRateDiv `18`, ReadError `1c` (zero), OpenFlags `20`; Paused is at `dc`.
The pinned DLL's advance helper writes LastFrameNum at `1800a209`, then
FrameNum at `1800a2b0`; Pause writes `dc` at `1800caa7`. LastFrameNum changes on
**NextFrame**, independently of the private decode-idempotence counter.
NextFrame wraps from Frames to 1 (`1800c2ae..1800c2f9`). Recomp-kit's duplicate
counts at `10/14` are incompatible with this AC1 layout and are not used.

GetFrameBuffersInfo is **not a YUV texture upload in the inspected path**.
The DLL export at `1800a560` writes a 32-bit **0x78-byte** descriptor: TotalFrames
at `00`, Y dimensions at `04/08`, chroma dimensions at `0c/10`, FrameNum at `14`,
and two 0x30-byte sets of four `{Allocate, Buffer32, BufferPitch}` planes at
`18/48`. Dimensions round up to multiples of eight; chroma uses ceil-half.
It writes FrameNum=0, Y/cR/cB Allocate=1, alpha Allocate from BinkType bit 20,
and zero pointers/pitches. The tested movies have no alpha, and two buffers.
AC1 uses count and Y dimensions in `00a50d70` to allocate ordinary textures,
selects one with descriptor FrameNum in `009e8e7d..009e8e87`, then locks via the
D3D texture vtable +0x34, calls CopyToBuffer, and unlocks via +0x38. There is no
RegisterFrameBuffers import or observed plane-pointer upload. The locked pitch
is supplied by D3D, not hardcoded; tightly packed 1280x720 BGRA is **5120** bytes.
Flags `0x80000005` mean COPYALL plus **BINKSURFACE32A**, little-endian BGRA.
Recomp-kit's surface type 3 is a different XRGB path; its converter also writes
zero alpha. Here opaque alpha is 255, as in the FFmpeg BGRA reference.

Private evidence: `calls.txt`, `player.asm`, `AssassinsCreed_Dx9.exe.asm`,
`binkw32.dll.asm`, `dll-prefix.asm`, `exports.txt`, `thunk-xrefs.txt`, and the
small `investigate.py`/`details.py`/`thunks.py` scripts beside them.

## Provider contract and runtime integration

`ac1_bink_native.h` is a C interface backed by C++17/FFmpeg. A provider owns a
map from **32-bit guest record addresses** to host-only decoder state. It never
puts host pointers into guest memory. The runtime supplies atomic-range guest
writes, a monotonic microsecond clock, copied PCM submission, and pause/resume/
stop callbacks. Frame copies take an already validated host mapping plus its
actual accessible size. Positive pitch, destination height and XY bounds are
checked before any write. Repeated DoFrame is idempotent; the display buffer
survives NextFrame. Wait/ShouldSkip use frame deadlines; skipped frames must
still go through the decoder. A last-frame NextFrame seeks back to frame 1.

Audio is decoded by selected **Bink track ID**, using FFmpeg stream IDs rather
than ordinal numbers. Packed/planar float becomes interleaved signed 16-bit PCM,
with per-track gain (32768 = unity). The callback gets rate, channels and sample
PTS; separate tracks need separate queued voices and host mixing/resampling.
No swresample library is needed in this provider. A NULL PCM callback is an
explicit silent-playback configuration. A refusing callback is a fatal decoder
error; errors never manufacture a completed record. Absent track IDs are
ignored, matching the silent LoadingScreen case and AC1's close-time resets.

Runtime integration in `ac1_runtime.c` and root CMake now follows this contract:

1. The host target links the provider and four private FFmpeg static libraries.
   The exact 14 imports dispatch before entering translated decoder code;
   unknown imports/unsupported flags must stop with `ac1_bink_error` and callsite.
   Keep the executable/DLL fingerprint checks. Audit GetProcAddress/export
   dispatch if permitting dynamic Bink calls beyond this observed import set.
2. One provider runs on the serialized guest thread. Open uses `ac1_files_open`
   with the existing read-only install-root mapping (including case-insensitive
   `videos` on Windows). `ac1_bink_open_input` reads/seeks through that service,
   so FFmpeg never resolves an unrestricted host path. Allocate at least
   `AC1_BINK_RECORD_BYTES` bytes in guest memory; call open with that address;
   return it in EAX, or free it and return null on an ordinary file-open failure.
   Close the provider entry before freeing the guest allocation. Destroy the
   provider before teardown of guest memory or the audio backend.
3. SetMemory records the guest allocator/free addresses, without executing them.
   Records are allocated from the runtime process heap and freed on BinkClose.
   This is equivalent for the observed AC1 ownership: its sole close call at
   `009e8bdb` is followed by clearing player+78, and no guest heap free of the
   returned BINK pointer was found. Host decoder allocations remain host-only.
   The translated DLL allocator-global assertion remains active only in the
   translated route. DLL mapping, fingerprint verification and PROCESS_ATTACH
   still execute in both modes.
4. Read SetSoundTrack's guest list with checked reads. Validate and map the
   CopyToBuffer destination for its entire rectangle, or stage/scatter it when
   guest allocations are not contiguous. GetFrameBuffersInfo writes its 0x78
   descriptor through the guest callback. Never treat plane words as host pointers.
5. SetSoundSystem/OpenDirectSound use a recognized guest factory/token
   handshake, backed by the provider's configured sink. Do not call the supplied
   guest factory as a native function. Route PCM to existing DirectSound
   streaming on Windows. An Android queue backend remains future work. Copy chunks synchronously;
   enforce queue capacity and propagate failures. The first Bink packet can
   contain audio ahead of the displayed frame. Apply pause/resume/stop to queued
   voices; live gain changes of already queued audio need backend volume control.
6. Translate provider status to the original stdcall ABI: Open returns the guest
   handle; Wait and ShouldSkip return their predicates. The inspected game
   ignores the other decode/copy/control return values; provider `1/-1` status
   is not itself the DLL's EAX convention. Perform the runtime's normal stack
   cleanup and preserve callee-saved registers. Native errors stop with the
   export name, record argument, provider diagnostic and runtime callsite.

## FFmpeg dependency and repeatable host test

Official source: [FFmpeg n8.1](https://github.com/FFmpeg/FFmpeg/tree/n8.1), commit
`9047fa1b084f76b1b4d065af2d743df1b40dfb56`. The source checkout is under ignored
work, downloaded only from that official repository. The ffmpeg.org release
download timed out; the official GitHub clone succeeded.

```powershell
git -c http.proxy= -c https.proxy= clone --depth 1 --branch n8.1 https://github.com/FFmpeg/FFmpeg.git work/bink_native/ffmpeg-src
& 'D:/Prog/dukemp_recomp/.venv/Scripts/python.exe' -B ports/ffmpeg/build_mingw_probe.py
& 'D:/Prog/dukemp_recomp/.venv/Scripts/python.exe' -B ports/ffmpeg/probe.py --bootstrap --frames 30
```

The tested fallback builds **191 unmodified upstream C sources** using MinGW
GCC 12.2.0 and native `ar`, with a pinned scalar-only configuration and generated
codec/demuxer/protocol registries. It enables only Bink demuxing, Bink video,
Bink DCT/RDFT audio, file IO and the avutil/avcodec/avformat/swscale libraries.
This deliberate test bootstrap is not a replacement for upstream configure on
other hosts or source versions. Headers/config, static archives and test binary
remain private in `work/bink_native/ffmpeg-native/`. `MINGW_BIN` can override the
compiler directory. No CLI subprocess is used inside the provider.

`build.sh mingw64` supplies the normal upstream configure/shared-library recipe
for use in an MSYS2 environment where Bash and MSYS GNU make can run. This
installation has `mingw32-make.exe` despite lacking MSYS make; that native make
is not assumed to understand upstream's POSIX paths and shell rules.
Both MSYS2 and Git Bash fail in the
current sandbox before configure, with denied signal-pipe/shared-memory IPC;
the standard shell recipe was therefore **not executed successfully** here.
Select an installed MSYS make with:
`MAKE=/usr/bin/make bash ports/ffmpeg/build.sh mingw64`. The minimal decoder's
configure name is **bink**, while its public codec name is **binkvideo**.
`probe.py --prefix <install-prefix>` also accepts a normal development install.

The installed Gyan **8.1.1 full_build** has CLI binaries/doc/presets, **no dev
headers or import libraries**, so it is used only as the independent reference.
A shared alternative needs matching avformat-62, avcodec-62, avutil-60 and
swscale-9 DLLs, their four MinGW `.dll.a` import libraries, public headers and
the generated `libavutil/avconfig.h`. None were borrowed from a mismatched CLI.

Actual host results (2026-10-29):

| Test | Result |
| --- | --- |
| Provider and test compile | C++17 `-Wall -Wextra -Werror`; C11 header compile passed |
| LoadingScreen, 30 BGRA frames | aggregate PSNR **49.316 dB**; full 100-frame tail and loop first-frame hash passed |
| UBI_LOGO, 30 BGRA frames | aggregate PSNR **49.601 dB** |
| UBI_LOGO selected ID 0 PCM | **165120 interleaved samples**, 48 kHz stereo; CLI-prefix max error **1**, RMS **0.01496928** s16 units |
| Controls/contracts | padded pitch/XY guards, invalid geometry/format, retained frame, exact wait boundary, pause/resume, late skip, unknown record, rejected writes/sink, IDs 0..3 and mute passed |

The hashes need not match because the fallback disables SIMD while the Gyan
reference enables it. Tests require per-frame PSNR >=45 dB, opaque alpha and PCM
error <=2 sample units. Reference commands use the Gyan CLI beside ffprobe on
PATH, not the older ffmpeg earlier on PATH. Full arguments, input/binary hashes,
results and per-frame checksums are in `work/bink_native/verification.json` and
`*.stdout.log`/`*.stderr.log`. Raw reference BGRA and PCM stay under work.

## Android and remaining limits

`build.sh android-arm64` documents the later upstream NDK 28.2.13676358 build:
`--target-os=android --arch=aarch64 --enable-cross-compile --enable-pic`, API 26
Clang, llvm-ar/nm/ranlib/strip, no symbol versioning and
`-Wl,-z,max-page-size=16384`. Set `ANDROID_NDK_HOME`; use `NDK_HOST_TAG=linux-x86_64`
on Linux. The Windows NDK's `.cmd` compiler wrappers may need direct clang plus
`--target=aarch64-linux-android26 --sysroot=<NDK>/toolchains/llvm/prebuilt/<host>/sysroot`
in a POSIX shell. Build all four libraries for ARM64; link with the runtime's
shared libc++ choice and package the shared libraries/required license material.

NDK 28.2 did compile the provider with C++17 and warnings-as-errors to
`work/bink_native/ac1_bink_native.aarch64.o`; llvm-readelf identifies ELF64,
little-endian **AArch64**, and llvm-nm shows the 16 unmangled C API definitions.
This is **compile-only**, against FFmpeg public headers and a little-endian
avconfig. No ARM64 FFmpeg link, APK, emulator or hardware playback was verified.

Remaining limits: exact original Bink color rounding is not promised; wall-clock
timing has no audio-device drift correction; gain applies to newly decoded PCM,
not audio already queued; backend mixing/resampling/capacity is external.
The test assets exercise DCT audio, not RDFT. General Bink alpha/YUV registration,
file-handle/memory opens, negative pitches, other surface formats and unrelated
open/copy flags are explicitly unsupported. OOM can raise a C++ allocation
exception; integration should define its process-failure policy. The original prototype results alone imply no game-rendering or hardware
milestone; see the integration execution evidence below.


## Host runtime switches and build

`AC1_BINK_TRANSLATED=1` selects the original translated imports for A/B. Leave
it unset for native decoding. The pinned DLL remains mapped and attached in
both cases. `AC1_BINK_SILENT=1` explicitly disables native Bink sound. If opening
the native DirectSound8 device fails, a diagnostic announces silent fallback;
decoding continues. The guest DirectSound device argument is recognized as
handshake metadata; audio uses an independent host device, not guest DS8 proxies.

The Windows sink uses one eight-second circular PCM voice per record/track,
serviced synchronously by Bink imports. It copies PCM into native locked buffers,
clears consumed spans, supports pause/resume/stop, and checks capacity. No provider
worker is spawned. Queued PCM already has gain applied; changes to already queued
audio and audio-device drift correction are future refinements. Long guest
stalls can underrun audio. Host DirectSound contract execution verifies API
submission/control, not audible output quality.

Root CMake caches `AC1_FFMPEG_PREFIX` (default
`work/bink_native/ffmpeg-native`). Generate that private prefix using the
bootstrap command above before `tools/build.ps1`. A conventional static FFmpeg
install with `include/` and `lib/lib{avformat,avcodec,swscale,avutil}.a` also works.
The provider's custom-input adapter is now used in the focused frame/PCM test.
Set `AC1_BINK_TEST_AUDIO=1` for that test to additionally submit actual decoded
PCM to the native DirectSound sink; it fails if opening/submitting audio fails.
`AC1_TRACE_IMPORTS=1` logs native Bink DoFrame/CopyToBuffer/PCM counters at most
once per second, plus totals at teardown.

The requested initial `tools/build.ps1 -BuildType RelWithDebInfo -Jobs 16` did
regenerate 83,878 lifted functions (83,891 discovered) once. In this sandbox,
the SDK's Ninja stalled during C/C++ compiler checks after creating the test
object, with no compiler child left running. Those processes were terminated.
The identical source configuration succeeded with MinGW Makefiles; the fallback
commands (no changes to tools/build.ps1) are:

```powershell
$cmake = "$env:LOCALAPPDATA/Android/Sdk/cmake/3.22.1/bin/cmake.exe"
& $cmake -S . -B build/host-make-relwithdebinfo -G 'MinGW Makefiles' -DCMAKE_MAKE_PROGRAM=C:/msys64/mingw64/bin/mingw32-make.exe -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe -DCMAKE_BUILD_TYPE=RelWithDebInfo
& $cmake --build build/host-make-relwithdebinfo --parallel 16
```

Android CMake configuration with NDK 28.2 defaults `AC1_NATIVE_BINK=OFF`; the
Android runtime C translation unit compiled successfully in this mode. This
preserves the translated Bink route until ARM64 FFmpeg libraries are supplied.


## Integration execution evidence (2026-10-29)

The RelWithDebInfo host executable linked successfully against all four static
FFmpeg libraries. The one startup execution used the fallback executable with
exactly the requested AC1 graphics/trace settings (the SDK Ninja issue prevented
using the build.ps1 `-Run` wrapper). DXVK log/cache paths were also set beneath
`work/bink_native` to keep private output in this worktree:

```powershell
$env:AC1_D3D9_LIBRARY='D:\Prog\ac1_recomp\work\d3d9_dxvk_probe\d3d9.dll'
$env:AC1_D3D9_FORCE_WINDOWED='1'
$env:AC1_TRACE_IMPORTS='1'
$env:DXVK_LOG_PATH=(Join-Path (Get-Location) 'work/bink_native')
$env:DXVK_STATE_CACHE_PATH=$env:DXVK_LOG_PATH
& .\build\host-make-relwithdebinfo\ac1_startup.exe 'C:\Program Files (x86)\GOG.com\Assassins Creed\AssassinsCreed_Dx9.exe' 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath work/startup.log
```

The pinned translated DLL's PROCESS_ATTACH succeeded. Native SetMemory recorded
allocator `009e8d90` and free `009e90d0`; SetSoundSystem recognized guest factory
`f1000400` and guest DirectSound object `300054e0`, and opened its native audio
sink. The rooted service opened, in order, LoadingScreen, UBI_LOGO,
Ubisoft_Presents, Warning_Disclaimer and DemoIntro. Final counters were
**DoFrame=3561, CopyToBuffer=36114, PCM-frames=5251200, audio=DirectSound**.
These prove native movie decoding, writes into the guest D3D staging buffer,
and successful PCM submission. They do not independently verify visible pixels
or audible output quality. The host run elapsed **157367.220 ms**.

It exited with runtime stop code 8 at **missing generated target `00a179e0`**,
called from **`00a173c0`** on guest thread **4104**, EIP **`00a173eb`**. This is
beyond the translated Bink decoder; no MMX Bink stop occurred. The new discovery
target was deliberately not added to config because config/lifter changes are
outside this task. Scheduler teardown reported five workers created, started
and joined, with 157406033 guest dispatches across workers. No startup, Ninja
or build processes were left running.

Private evidence: `work/startup.log`, `work/bink_native/startup-native.log`,
`runtime-verification.json`, `make-build.log`, `host-build.log` (Ninja failure),
`host-audio-provider-tests.log`, `*.test.stdout.log`, `android-make-config.log`,
`android-runtime-compile.log`, and `arm64-inspection.log`. The source revision
and binary/source fingerprints are recorded in runtime-verification.json.
All five movie opens and final counts come from this single live game run;
no A/B translated startup or Android hardware run was performed.

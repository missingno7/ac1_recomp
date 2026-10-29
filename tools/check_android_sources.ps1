[CmdletBinding()]
param(
    [string]$AndroidSdk = "$env:LOCALAPPDATA/Android/Sdk",
    [string]$GeneratedDir = "$PSScriptRoot/../work/lift_initial",
    [string]$BinkGeneratedDir = "$PSScriptRoot/../work/lift_bink",
    [string]$DxvkInclude = "$PSScriptRoot/../work/dxvk/prepared/include/native",
    [string]$FFmpegInclude = "$PSScriptRoot/../work/bink_native/ffmpeg-android-arm64/install/include",
    [string]$FFmpegConfigInclude = $FFmpegInclude
)
# Deliberately no CMake/Gradle/build wrapper, no generated C, no object output.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
$clang = "$AndroidSdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
$common = @('--target=aarch64-linux-android29', '-fsyntax-only', '-fPIC',
    '-Wall', '-Wextra', '-Werror=return-type', '-Werror=implicit-function-declaration',
    '-fwrapv', '-fno-strict-aliasing', '-ffp-contract=off',
    '-DAC1_HOST_D3D9=1', '-DAC1_DIRECT_MEMORY=1', '-DAC1_PROFILE_HOOKS=1', '-DRECOMP_FS_SIMULATED',
    "-I$root/src/runtime", "-I$root/src/platform", "-I$GeneratedDir", "-I$BinkGeneratedDir",
    "-I$DxvkInclude/windows", "-I$DxvkInclude/directx", "-I$FFmpegConfigInclude", "-I$FFmpegInclude")
$sources = @(
    'src/runtime/ac1_runtime.c', 'src/runtime/ac1_profile.c', 'src/runtime/ac1_network.c',
    'src/runtime/ac1_files.c', 'src/runtime/ac1_crypto.c', 'src/runtime/guest_clock.c',
    'src/runtime/guest_memory.c', 'src/runtime/pe32_loader.c', 'src/runtime/pcrecomp_heap.c',
    'src/platform/ac1_audio_win32.c', 'src/platform/ac1_ds8_portable.cpp',
    'src/platform/ac1_dinput8_portable.cpp', 'src/platform/ac1_input_script.cpp',
    'src/platform/ac1_window_android.c', 'src/platform/ac1_bink_native.cpp',
    'src/graphics/ac1_d3d9_bridge.cpp', 'android/app/src/main/cpp/native_bridge.c')
$failed = @()
foreach ($source in $sources) {
    Write-Output "ARM64 syntax: $source"
    $extra = @('-std=c11')
    if ($source.EndsWith('.cpp')) { $extra = @('-std=c++17') }
    if ($source -eq 'src/runtime/ac1_runtime.c') { $extra += '-DAC1_NATIVE_BINK=1' }
    & $clang @common @extra "$root/$source"
    if ($LASTEXITCODE -ne 0) { $failed += $source }
}
if ($failed.Count) { throw "ARM64 syntax failed: $($failed -join ', ')" }
Write-Output "PASS: $($sources.Count) runtime/platform/JNI sources; zero generated game functions compiled."

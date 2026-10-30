[CmdletBinding()]
param(
    [string]$AndroidSdk = "$env:LOCALAPPDATA/Android/Sdk",
    [string]$HostBin = 'C:/msys64/mingw64/bin',
    [string]$GeneratedDir = '',
    [string]$BinkGeneratedDir = ''
)
# Small contracts and one-file syntax checks only. No game build or execution.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
# Windows PowerShell leaves $PSScriptRoot empty in parameter defaults.
if (-not $GeneratedDir) { $GeneratedDir = "$root/work/lift_initial" }
if (-not $BinkGeneratedDir) { $BinkGeneratedDir = "$root/work/lift_bink" }
$output = "$root/work/audio"
New-Item -ItemType Directory -Force $output | Out-Null
function Check([string]$exe, [string[]]$arguments) {
    Write-Output "$exe $($arguments -join ' ')"
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "Check failed ($LASTEXITCODE): $exe" }
}
Push-Location $root
try {
    $hostFlags = @('-std=c++17','-O1','-Wall','-Wextra','-DAC1_PORTABLE_TEST',
        '-DAC1_HOST_D3D9=1','-D_Thread_local=thread_local','-Isrc/runtime','-Isrc/platform')
    Check "$HostBin/g++.exe" ($hostFlags + @('tests/audio_portable_contract.cpp',
        'src/platform/ac1_ds8_portable.cpp','-o',"$output/audio_portable_contract.exe"))
    Check "$output/audio_portable_contract.exe" @()
    Check "$HostBin/gcc.exe" @('-std=c11','-DAC1_PORTABLE_TEST','-Isrc/runtime','-Isrc/platform',
        '-c','src/platform/ac1_window_android.c','-o',"$output/window.o")
    Check "$HostBin/g++.exe" ($hostFlags + @('tests/android_portable_contract.cpp',
        'src/platform/ac1_ds8_portable.cpp','src/platform/ac1_dinput8_portable.cpp',
        'src/platform/ac1_input_script.cpp',"$output/window.o",'-o',"$output/android_portable_contract.exe"))
    Check "$output/android_portable_contract.exe" @()
    $ndk = "$AndroidSdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/bin"
    $sources = @('src/platform/ac1_ds8_portable.cpp','src/platform/ac1_audio_aaudio.cpp',
        'src/platform/ac1_audio_win32.c','src/runtime/ac1_runtime.c',
        'android/app/src/main/cpp/native_bridge.c','tests/audio_portable_contract.cpp')
    foreach ($abi in @('aarch64','x86_64')) {
        foreach ($source in $sources) {
            $compiler = "$ndk/clang.exe"
            $flags = @("--target=$abi-linux-android29",'-fsyntax-only','-fPIC','-Wall','-Wextra',
                '-Werror=return-type','-DAC1_HOST_D3D9=1','-DAC1_DIRECT_MEMORY=1',
                '-DAC1_PROFILE_HOOKS=1','-DRECOMP_FS_SIMULATED','-Isrc/runtime','-Isrc/platform',
                "-I$GeneratedDir","-I$BinkGeneratedDir")
            if ($source.EndsWith('.cpp')) { $compiler = "$ndk/clang++.exe"; $flags += '-std=c++17' }
            else { $flags += @('-std=c11','-Werror=implicit-function-declaration') }
            if ($source -eq 'src/runtime/ac1_runtime.c') { $flags += '-DAC1_NATIVE_BINK=1' }
            Check $compiler ($flags + $source)
        }
        # Link only the two host-owned audio files against the real API29 NDK
        # stubs: no runtime, generated code, FFmpeg archives or game assets.
        Check "$ndk/clang++.exe" @("--target=$abi-linux-android29",'-std=c++17','-fPIC','-shared',
            '-Wl,--no-undefined','src/platform/ac1_ds8_portable.cpp','src/platform/ac1_audio_aaudio.cpp',
            '-laaudio','-o',"$output/audio-$abi.so")
        Check "$ndk/llvm-readelf.exe" @('-h','-d',"$output/audio-$abi.so")
    }
    Write-Output 'PASS: both host contracts; 12 single-file NDK/API29 checks; two audio-only NDK shared links inspected.'
} finally { Pop-Location }

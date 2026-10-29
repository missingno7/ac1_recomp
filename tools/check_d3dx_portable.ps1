[CmdletBinding()]
param(
    [string]$AndroidSdk = "$env:LOCALAPPDATA/Android/Sdk",
    [string]$DxvkInclude = "$PSScriptRoot/../work/dxvk/prepared/include/native",
    [string]$HostCompiler = 'C:/msys64/mingw64/bin/g++.exe'
)
# Sequential, focused checks only. No game/dependency build, Gradle or game run.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
$out = Join-Path $root 'work/d3dx_portable'
New-Item -ItemType Directory -Force $out | Out-Null
$clang = "$AndroidSdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe"
foreach ($required in @($HostCompiler, $clang, "$DxvkInclude/directx/d3d9.h")) {
    if (!(Test-Path -LiteralPath $required)) { throw "Missing check input: $required" }
}
function Invoke-Check([string]$Program, [string[]]$Arguments, [string]$Log) {
    & $Program @Arguments *> $Log
    $result = $LASTEXITCODE
    if ($result) {
        Get-Content -LiteralPath $Log
        throw "$Program exited $result; see $Log"
    }
}
Invoke-Check $HostCompiler @('-std=c++17', '-O0', '-Wall', '-Wextra', '-Werror',
    "$root/tests/d3dx_portable_contract.cpp", '-o', "$out/contract.exe",
    '-static-libgcc', '-static-libstdc++') "$out/host_compile.log"
Invoke-Check "$out/contract.exe" @() "$out/contract.log"
Get-Content -LiteralPath "$out/contract.log"
Invoke-Check $HostCompiler @('-std=c++17', '-fsyntax-only', '-Wall', '-Wextra', '-Werror',
    "$root/src/graphics/ac1_d3d9_bridge.cpp") "$out/windows_bridge_syntax.log"
Write-Output 'PASS: Windows bridge syntax (native D3DX forwarding)'
Invoke-Check $clang @('--target=aarch64-linux-android29', '-fsyntax-only', '-std=c++17',
    '-Wall', '-Wextra', '-Werror', "-I$DxvkInclude/windows", "-I$DxvkInclude/directx",
    "$root/src/graphics/ac1_d3d9_bridge.cpp") "$out/arm64_syntax.log"
Write-Output 'PASS: NDK 28.2 ARM64/API29 bridge syntax (portable D3DX)'

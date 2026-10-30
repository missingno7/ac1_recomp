[CmdletBinding()]
param(
    [string]$AndroidSdk = $(if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }),
    [string]$AndroidNdk = '28.2.13676358',
    [string]$Serial = '',
    [string]$OutputDirectory = ''
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (!$OutputDirectory) { $OutputDirectory = Join-Path $Root 'work/render/vulkan-probe' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$Toolchain = Join-Path $AndroidSdk "ndk/$AndroidNdk/toolchains/llvm/prebuilt/windows-x86_64"
$Compiler = Join-Path $Toolchain 'bin/clang.exe'
$Executable = Join-Path $OutputDirectory 'vulkan_probe'
& $Compiler --target=aarch64-linux-android29 "--sysroot=$Toolchain/sysroot" -O2 -Wall -Wextra `
    (Join-Path $PSScriptRoot 'vulkan_probe.c') -lvulkan -o $Executable
if ($LASTEXITCODE) { throw 'Vulkan probe compilation failed' }
Write-Host "Built ARM64 probe: $Executable"
if (!$Serial) { return }
$Adb = Join-Path $AndroidSdk 'platform-tools/adb.exe'
$Remote = '/data/local/tmp/ac1-vulkan-probe'
& $Adb -s $Serial push $Executable $Remote
if ($LASTEXITCODE) { throw 'Vulkan probe push failed' }
try {
    & $Adb -s $Serial shell chmod 755 $Remote
    if ($LASTEXITCODE) { throw 'Vulkan probe chmod failed' }
    $Lines = & $Adb -s $Serial shell $Remote
    $ProbeExit = $LASTEXITCODE
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'capabilities.txt'), ($Lines -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
    $Lines | Write-Output
    if ($ProbeExit -notin @(0, 2)) { throw "Vulkan probe execution failed: $ProbeExit" }
} finally {
    & $Adb -s $Serial shell rm -f $Remote
}
# 2 means a successful capability query found no DXVK 3.1 candidate.
exit $ProbeExit

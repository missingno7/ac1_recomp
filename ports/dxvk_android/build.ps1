param(
    [string]$AndroidSdk = $(if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }),
    [string]$AndroidNdk = '28.2.13676358',
    [ValidateSet('arm64-v8a', 'x86_64')][string]$Abi = 'arm64-v8a',
    [ValidateRange(1,64)][int]$Jobs = 4,
    [ValidateSet('Ninja', 'Make')][string]$Builder = 'Make',
    [string]$Make = 'C:/msys64/mingw64/bin/mingw32-make.exe'
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$NdkRoot = Join-Path $AndroidSdk "ndk\$AndroidNdk"
$Cmake = Join-Path $AndroidSdk 'cmake\3.22.1\bin\cmake.exe'
$Ninja = Join-Path $AndroidSdk 'cmake\3.22.1\bin\ninja.exe'
$Glslc = Join-Path $NdkRoot 'shader-tools\windows-x86_64\glslc.exe'
$SpirvVal = Join-Path $NdkRoot 'shader-tools\windows-x86_64\spirv-val.exe'
$Python = (Get-Command python.exe -ErrorAction Stop).Source
$Dependency = Join-Path $Root 'work\deps\dxvk-3.1-turnip'
$Prepared = Join-Path $Root 'work\dxvk\prepared-3.1'
$AbiTag = if ($Abi -eq 'arm64-v8a') { 'arm64' } else { 'x86_64' }
$Build = Join-Path $Root "work\build\dxvk31-android-$AbiTag-$($Builder.ToLowerInvariant())"

foreach ($Required in @($Cmake, $Ninja, $Glslc, $SpirvVal)) {
    if (!(Test-Path -LiteralPath $Required)) { throw "Required build tool not found: $Required" }
}
if (!(Test-Path -LiteralPath (Join-Path $Dependency '.git'))) {
    New-Item -ItemType Directory -Force -Path (Split-Path $Dependency) | Out-Null
    git clone --depth 1 --branch v3.1 --recurse-submodules https://github.com/doitsujin/dxvk.git $Dependency
    if ($LASTEXITCODE -ne 0) { throw 'Official DXVK 3.1 clone failed' }
}
$Revision = (git -C $Dependency rev-parse HEAD).Trim()
if ($Revision -ne '70d7508c01201ed3d4bfb33da42ba834eafe3857') { throw "Unexpected DXVK revision: $Revision" }

$PrepareScript = Join-Path $PSScriptRoot 'prepare.py'
$PreparedMarker = Join-Path $Prepared 'ANDROID_PREPARE_SHA256'
$ExpectedPrepareHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $PrepareScript).Hash.ToLowerInvariant()
$PreparedHash = if (Test-Path -LiteralPath $PreparedMarker) { (Get-Content -LiteralPath $PreparedMarker -Raw).Trim() } else { '' }
if ($PreparedHash -ne $ExpectedPrepareHash) {
    & $Python $PrepareScript $Dependency $Prepared
    if ($LASTEXITCODE -ne 0) { throw 'DXVK Android preparation failed' }
}
& $Python (Join-Path $PSScriptRoot 'shaders.py') $Prepared --glslc $Glslc --spirv-val $SpirvVal
if ($LASTEXITCODE -ne 0) { throw 'DXVK shader generation failed' }

$Generator = if ($Builder -eq 'Make') { 'MinGW Makefiles' } else { 'Ninja' }
$BuildTool = if ($Builder -eq 'Make') { $Make } else { $Ninja }
& $Cmake -S $PSScriptRoot -B $Build -G $Generator `
    "-DCMAKE_MAKE_PROGRAM=$BuildTool" `
    "-DCMAKE_TOOLCHAIN_FILE=$NdkRoot\build\cmake\android.toolchain.cmake" `
    "-DANDROID_ABI=$Abi" '-DANDROID_PLATFORM=29' `
    "-DDXVK_SOURCE=$Prepared" '-DCMAKE_BUILD_TYPE=Release' '-DANDROID_STL=c++_static' `
    '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'
if ($LASTEXITCODE -ne 0) { throw 'DXVK CMake configure failed' }
& $Cmake --build $Build --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw "DXVK $Abi build failed" }
Write-Host "Built $Abi DXVK dependency: $(Join-Path $Build 'libac1_dxvk_android.so')"
$Published = Join-Path $Root "work/build/dxvk-android-$AbiTag"
New-Item -ItemType Directory -Force -Path $Published | Out-Null
& (Join-Path $NdkRoot 'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe') --strip-unneeded `
    -o (Join-Path $Published 'libac1_dxvk_android.so') (Join-Path $Build 'libac1_dxvk_android.so')
if ($LASTEXITCODE) { throw 'DXVK release library stripping failed' }

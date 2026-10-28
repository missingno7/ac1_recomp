param(
    [string]$AndroidSdk = $(if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }),
    [string]$AndroidNdk = '28.2.13676358'
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$NdkRoot = Join-Path $AndroidSdk "ndk\$AndroidNdk"
$Cmake = Join-Path $AndroidSdk 'cmake\3.22.1\bin\cmake.exe'
$Ninja = Join-Path $AndroidSdk 'cmake\3.22.1\bin\ninja.exe'
$Glslc = Join-Path $NdkRoot 'shader-tools\windows-x86_64\glslc.exe'
$SpirvVal = Join-Path $NdkRoot 'shader-tools\windows-x86_64\spirv-val.exe'
$Python = (Get-Command python.exe -ErrorAction Stop).Source
$Dependency = Join-Path $Root 'work\deps\dxvk-native'
$Prepared = Join-Path $Root 'work\dxvk\prepared'
$Build = Join-Path $Root 'work\build\dxvk-android-arm64'

foreach ($Required in @($Cmake, $Ninja, $Glslc, $SpirvVal)) {
    if (!(Test-Path -LiteralPath $Required)) { throw "Required build tool not found: $Required" }
}
if (!(Test-Path -LiteralPath (Join-Path $Dependency '.git'))) {
    New-Item -ItemType Directory -Force -Path (Split-Path $Dependency) | Out-Null
    git clone --depth 1 --branch native-1.9.2b https://github.com/Joshua-Ashton/dxvk-native.git $Dependency
    if ($LASTEXITCODE -ne 0) { throw 'DXVK Native clone failed' }
}
$Revision = (git -C $Dependency rev-parse HEAD).Trim()
if ($Revision -ne 'c8dc91fabd00cac11d697ccf07426e798393cd40') { throw "Unexpected DXVK revision: $Revision" }

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

& $Cmake -S $PSScriptRoot -B $Build -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$Ninja" `
    "-DCMAKE_TOOLCHAIN_FILE=$NdkRoot\build\cmake\android.toolchain.cmake" `
    '-DANDROID_ABI=arm64-v8a' '-DANDROID_PLATFORM=26' `
    "-DDXVK_SOURCE=$Prepared" '-DCMAKE_BUILD_TYPE=Release'
if ($LASTEXITCODE -ne 0) { throw 'DXVK CMake configure failed' }
& $Cmake --build $Build --parallel
if ($LASTEXITCODE -ne 0) { throw 'DXVK ARM64 build failed' }
Write-Host "Built ARM64 DXVK dependency: $(Join-Path $Build 'libac1_dxvk_android.so')"

[CmdletBinding()]
param(
    [string]$AndroidSdk = $env:ANDROID_HOME,
    [string]$Python = 'D:\Prog\dukemp_recomp\.venv\Scripts\python.exe',
    [ValidateRange(1,64)][int]$Jobs = 2,
    # x86_64 is for the emulator (native speed under WHPX); arm64-v8a for phones.
    [ValidateSet('arm64-v8a', 'x86_64')][string]$Abi = 'arm64-v8a',
    [ValidateSet('Gradle', 'Make')][string]$NativeBuilder = 'Gradle',
    [string]$Make = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$abiTag = if ($Abi -eq 'arm64-v8a') { 'arm64' } else { 'x86_64' }
# Check the two external native providers before generating any game code.
foreach ($component in @('avformat', 'avcodec', 'swscale', 'avutil')) {
    $archive = Join-Path $repoRoot "work\bink_native\ffmpeg-android-$abiTag\install\lib\lib$component.a"
    if (-not (Test-Path -LiteralPath $archive)) {
        throw "Missing $Abi FFmpeg $component. See docs/android.md: ports/ffmpeg/build.sh android-$abiTag."
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $repoRoot "work\build\dxvk-android-$abiTag\libac1_dxvk_android.so"))) {
    throw "Missing Android DXVK provider. Run ports/dxvk_android/build.ps1 -Abi $Abi after any benchmark."
}
if ([string]::IsNullOrWhiteSpace($AndroidSdk)) {
    $AndroidSdk = Join-Path $env:LOCALAPPDATA 'Android\Sdk'
}
if (-not (Test-Path -LiteralPath (Join-Path $AndroidSdk 'platforms\android-35\android.jar'))) {
    throw "Android SDK platform 35 was not found under '$AndroidSdk'. Pass -AndroidSdk or set ANDROID_HOME."
}
& (Join-Path $PSScriptRoot 'build.ps1') -PrepareOnly -Python $Python
if ($LASTEXITCODE -ne 0) { throw 'Native input preparation failed' }

function Get-JavaMajor([string]$JavaInstallRoot) {
    $releaseFile = Join-Path $JavaInstallRoot 'release'
    if (-not (Test-Path -LiteralPath (Join-Path $JavaInstallRoot 'bin\java.exe'))) { return 0 }
    if (Test-Path -LiteralPath $releaseFile) {
        $releaseLine = Get-Content -LiteralPath $releaseFile | Where-Object { $_ -match '^JAVA_VERSION=' } | Select-Object -First 1
        if ($releaseLine -match '"(?<version>\d+)') { return [int]$Matches.version }
    }
    return 0
}

$javaCandidates = @()
if (-not [string]::IsNullOrWhiteSpace($env:JAVA_HOME)) { $javaCandidates += $env:JAVA_HOME }
$javaCandidates += (Join-Path $env:ProgramFiles 'Java\jdk-17')
$javaCandidates += (Join-Path $env:ProgramFiles 'Java\jdk-21')
$adoptium = Join-Path $env:ProgramFiles 'Eclipse Adoptium'
if (Test-Path -LiteralPath $adoptium) {
    $javaCandidates += Get-ChildItem -LiteralPath $adoptium -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^jdk-(17|21)\.' } | ForEach-Object FullName
}
$javaHome = $javaCandidates | Select-Object -Unique | Where-Object { (Get-JavaMajor $_) -in @(17, 21) } | Select-Object -First 1
if (-not $javaHome) {
    throw 'JDK 17 or 21 was not found. Set JAVA_HOME to an installed JDK 17 or 21 before building.'
}

$oldJavaHome = $env:JAVA_HOME
$oldAndroidHome = $env:ANDROID_HOME
$oldAndroidSdkRoot = $env:ANDROID_SDK_ROOT
$oldPath = $env:Path
$oldParallel = $env:CMAKE_BUILD_PARALLEL_LEVEL
$env:JAVA_HOME = $javaHome
$env:ANDROID_HOME = $AndroidSdk
$env:ANDROID_SDK_ROOT = $AndroidSdk
$env:Path = "$(Join-Path $javaHome 'bin');$env:Path"
$env:CMAKE_BUILD_PARALLEL_LEVEL = "$Jobs"

$buildExit = 0
$timer = [System.Diagnostics.Stopwatch]::StartNew()
$nativeLibrary = $null
$prebuiltArgument = @()
$apkPath = Join-Path $repoRoot 'android\app\build\outputs\apk\debug\app-debug.apk'
Push-Location (Join-Path $repoRoot 'android')
try {
    if ($NativeBuilder -eq 'Make') {
        if (-not $Make) { $Make = (Get-Command mingw32-make.exe -ErrorAction Stop).Source }
        $cmake = Join-Path $AndroidSdk 'cmake/3.22.1/bin/cmake.exe'
        $ndkToolchain = Join-Path $AndroidSdk 'ndk/28.2.13676358/build/cmake/android.toolchain.cmake'
        $nativeBuild = Join-Path $repoRoot "work/build/ac1-android-$abiTag-relwithdebinfo"
        $jniRoot = Join-Path $nativeBuild 'jniLibs'
        $jniAbi = Join-Path $jniRoot $Abi
        & $cmake -S (Join-Path $repoRoot 'android/app/src/main/cpp') -B $nativeBuild `
            -G 'MinGW Makefiles' "-DCMAKE_MAKE_PROGRAM=$Make" `
            "-DCMAKE_TOOLCHAIN_FILE=$ndkToolchain" "-DANDROID_ABI=$Abi" `
            -DANDROID_PLATFORM=29 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=RelWithDebInfo `
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$jniAbi" `
            -DAC1_NATIVE_BINK=ON -DAC1_DIRECT_MEMORY=ON -DAC1_FORCE_INLINE_ACCESSORS=ON
        if ($LASTEXITCODE) { throw 'Android native CMake configuration failed' }
        & $cmake --build $nativeBuild --parallel $Jobs
        if ($LASTEXITCODE) { throw 'Android native Make build failed' }
        $nativeLibrary = Join-Path $jniAbi 'libac1_android.so'
        Copy-Item -LiteralPath (Join-Path $repoRoot "work/build/dxvk-android-$abiTag/libac1_dxvk_android.so") -Destination $jniAbi
        $prebuiltArgument = @("-Pac1PrebuiltNativeDir=$jniRoot")
    }
    # Recreate the ZIP: incremental packaging can retain large gaps after a .so shrinks.
    if (Test-Path -LiteralPath $apkPath) { Remove-Item -LiteralPath $apkPath -Force }
    & .\gradlew.bat --no-daemon "--max-workers=$Jobs" "-Pac1Abi=$Abi" @prebuiltArgument assembleDebug
    $buildExit = $LASTEXITCODE
} finally {
    Pop-Location
    $env:JAVA_HOME = $oldJavaHome
    $env:ANDROID_HOME = $oldAndroidHome
    $env:ANDROID_SDK_ROOT = $oldAndroidSdkRoot
    $env:Path = $oldPath
    $env:CMAKE_BUILD_PARALLEL_LEVEL = $oldParallel
}
$timer.Stop()
if ($buildExit -ne 0) { exit $buildExit }

Write-Output "Built local diagnostic APK ($Abi): $apkPath"
Write-Output ("Android build time: {0:n1} seconds" -f $timer.Elapsed.TotalSeconds)

# Gradle strips the packaged copy; retain native symbols outside the APK.
if (-not $nativeLibrary) {
    $nativeLibrary = Get-ChildItem -Path (Join-Path $repoRoot "android/app/build/intermediates/cxx/RelWithDebInfo/*/obj/$Abi/libac1_android.so") |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $nativeLibrary -or -not (Test-Path -LiteralPath $nativeLibrary)) {
    throw 'The unstripped Android native library was not found'
}
$symbols = Join-Path $repoRoot "work/android_symbols/$Abi"
New-Item -ItemType Directory -Force -Path $symbols | Out-Null
Copy-Item -LiteralPath $nativeLibrary -Destination (Join-Path $symbols 'libac1_android.so')
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($apkPath)
try {
    $entry = $archive.GetEntry("lib/$Abi/libac1_android.so")
    if (-not $entry) { throw "APK has no $Abi runtime library" }
    $metrics = [ordered]@{
        abi = $Abi; native_build_type = 'RelWithDebInfo'; native_builder = $NativeBuilder
        build_seconds = $timer.Elapsed.TotalSeconds
        apk_bytes = (Get-Item -LiteralPath $apkPath).Length
        unstripped_so_bytes = (Get-Item -LiteralPath $nativeLibrary).Length
        packaged_so_bytes = $entry.Length
        symbols = (Join-Path $symbols 'libac1_android.so')
    }
    $metrics | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $symbols 'build-metrics.json')
    Write-Output "Native library: $($metrics.packaged_so_bytes) packaged bytes; $($metrics.unstripped_so_bytes) unstripped bytes"
    Write-Output "Native symbols: $symbols"
} finally { $archive.Dispose() }
exit 0

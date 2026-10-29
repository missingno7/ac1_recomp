[CmdletBinding()]
param(
    [string]$AndroidSdk = $env:ANDROID_HOME,
    [string]$Python = 'D:\Prog\dukemp_recomp\.venv\Scripts\python.exe',
    [ValidateRange(1,64)][int]$Jobs = 2
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
# Check the two external native providers before generating any game code.
foreach ($component in @('avformat', 'avcodec', 'swscale', 'avutil')) {
    $archive = Join-Path $repoRoot "work\bink_native\ffmpeg-android-arm64\install\lib\lib$component.a"
    if (-not (Test-Path -LiteralPath $archive)) {
        throw "Missing ARM64 FFmpeg $component. See docs/android.md: ports/ffmpeg/build.sh android-arm64."
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $repoRoot 'work\build\dxvk-android-arm64\libac1_dxvk_android.so'))) {
    throw 'Missing Android DXVK provider. Run ports/dxvk_android/build.ps1 after any benchmark.'
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
Push-Location (Join-Path $repoRoot 'android')
try {
    & .\gradlew.bat --no-daemon "--max-workers=$Jobs" assembleDebug
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

$apkPath = Join-Path $repoRoot 'android\app\build\outputs\apk\debug\app-debug.apk'
Write-Output "Built local diagnostic APK: $apkPath"
Write-Output ("Android build time: {0:n1} seconds" -f $timer.Elapsed.TotalSeconds)

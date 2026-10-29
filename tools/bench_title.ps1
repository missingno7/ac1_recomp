# Startup benchmark without input: time from the first presents (loading
# screen) to the title screen, then idle title-screen frame rate.
# Runs ac1_startup.exe with a timeout and parses the D3D9 present trace.
param(
    [string]$BuildDir = 'build/host-relwithdebinfo',
    [int]$TimeoutSeconds = 150,
    [string]$Log = 'work/bench_title.log',
    [string]$GameExe = 'C:\Program Files (x86)\GOG.com\Assassins Creed\AssassinsCreed_Dx9.exe'
)
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $ProjectRoot
try {
    $env:AC1_D3D9_LIBRARY = (Resolve-Path work\d3d9_dxvk_probe\d3d9.dll).Path
    $env:AC1_D3D9_FORCE_WINDOWED = '1'
    New-Item -ItemType Directory -Force work\host_dxvk_run | Out-Null
    $env:DXVK_LOG_PATH = (Resolve-Path work\host_dxvk_run).Path
    $env:AC1_DISPATCH_LIMIT = '0'
    $env:AC1_D3D9_TRACE_FRAME = '1'
    $env:AC1_HIDE_FILES = 'videos\UBI_LOGO.bik;videos\Ubisoft_Presents.bik;videos\Warning_Disclaimer.bik;videos\DemoIntro.bik'
    Remove-Item Env:AC1_TRACE_IMPORTS -ErrorAction SilentlyContinue
    Remove-Item Env:AC1_D3D9_CAPTURE_DIR -ErrorAction SilentlyContinue
    $err = Join-Path $ProjectRoot "$Log.err"
    $process = Start-Process -FilePath (Join-Path $ProjectRoot "$BuildDir/ac1_startup.exe") `
        -ArgumentList "`"$GameExe`"" -RedirectStandardError $err -RedirectStandardOutput (Join-Path $ProjectRoot $Log) `
        -PassThru -NoNewWindow
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) { Stop-Process -Id $process.Id -Force }
    $ticks = @{}
    foreach ($line in Get-Content $err) {
        if ($line -match 'present_calls=(\d+) draw_calls=(\d+) t_ms=(\d+)') { $ticks[[int]$Matches[1]] = [int64]$Matches[3] }
    }
    $stop = Select-String -Path $err -Pattern 'guest stop' | Select-Object -First 1
    "presents seen: $(($ticks.Keys | Sort-Object) -join ',')"
    if ($ticks[2] -and $ticks[128]) { 'loading (present 2 -> 128): {0:N1} s' -f (($ticks[128] - $ticks[2]) / 1000.0) }
    if ($ticks[128] -and $ticks[256]) { 'title fps (128 -> 256): {0:N1}' -f (128000.0 / ($ticks[256] - $ticks[128])) }
    if ($ticks[256] -and $ticks[512]) { 'title fps (256 -> 512): {0:N1}' -f (256000.0 / ($ticks[512] - $ticks[256])) }
    if ($stop) { "stop: $($stop.Line)" }
} finally { Pop-Location }

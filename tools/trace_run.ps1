# Short traced startup for divergence analysis: runs ac1_startup.exe with
# import tracing for a fixed time and keeps stderr. Same environment as
# tools/bench_title.ps1 so traced and benchmark runs follow the same path.
param(
    [string]$BuildDir = 'build/host-direct',
    [int]$Seconds = 40,
    [string]$Log = 'work/trace_run.log',
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
    $env:AC1_TRACE_IMPORTS = '1'
    $env:AC1_HIDE_FILES = 'videos\UBI_LOGO.bik;videos\Ubisoft_Presents.bik;videos\Warning_Disclaimer.bik;videos\DemoIntro.bik'
    $process = Start-Process -FilePath (Join-Path $ProjectRoot "$BuildDir/ac1_startup.exe") `
        -ArgumentList "`"$GameExe`"" -RedirectStandardError (Join-Path $ProjectRoot $Log) `
        -RedirectStandardOutput (Join-Path $ProjectRoot "$Log.out") -PassThru -NoNewWindow
    if (-not $process.WaitForExit($Seconds * 1000)) { Stop-Process -Id $process.Id -Force }
    Select-String -Path $Log -Pattern 'present_calls=(64|128|256) ' | ForEach-Object { $_.Line }
} finally { Pop-Location }

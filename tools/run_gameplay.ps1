param(
    [string]$Script = 'tools/input/to_gameplay.txt',
    [ValidateRange(0, 86400)][int]$Seconds = 0,
    [Alias('Env')][hashtable]$Environment = @{},
    [string]$GameExe = 'C:\Program Files (x86)\GOG.com\Assassins Creed\AssassinsCreed_Dx9.exe',
    [string]$OutputDirectory = '',
    [ValidateRange(1, 64)][int]$Jobs = 6,
    # Reuse a completed build for repeated runs of exactly the same binary.
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $ProjectRoot
$Process = $null
$PreviousEnvironment = @{}
$BuildProcess = $null
function Stop-OwnedProcessTree($OwnedProcess) {
    if (-not $OwnedProcess -or $OwnedProcess.HasExited) { return }
    # taskkill /T uses process enumeration that is denied in some sandboxes.
    # Toolhelp can enumerate our descendants without WMI or elevation.
    if (-not ('AC1InputProcessTree' -as [type])) {
        Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class AC1InputProcessTree {
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    struct Entry {
        public uint size, usage, pid;
        public UIntPtr heap;
        public uint module, threads, parent;
        public int priority;
        public uint flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=260)] public string name;
    }
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint pid);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern bool Process32FirstW(IntPtr snapshot, ref Entry entry);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern bool Process32NextW(IntPtr snapshot, ref Entry entry);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    public static int[] Descendants(int root) {
        IntPtr snapshot = CreateToolhelp32Snapshot(2, 0);
        if (snapshot == new IntPtr(-1)) throw new Win32Exception();
        var rows = new List<KeyValuePair<int,int>>();
        try {
            Entry entry = new Entry(); entry.size = (uint)Marshal.SizeOf(typeof(Entry));
            bool more = Process32FirstW(snapshot, ref entry);
            while (more) {
                rows.Add(new KeyValuePair<int,int>((int)entry.pid, (int)entry.parent));
                more = Process32NextW(snapshot, ref entry);
            }
        } finally { CloseHandle(snapshot); }
        var ids = new List<int>(); ids.Add(root);
        for (int i=0; i<ids.Count; ++i)
            foreach (var row in rows)
                if (row.Value == ids[i] && !ids.Contains(row.Key)) ids.Add(row.Key);
        return ids.ToArray();
    }
}
"@
    }
    foreach ($ChildId in [AC1InputProcessTree]::Descendants($OwnedProcess.Id)) {
        Stop-Process -Id $ChildId -Force -ErrorAction SilentlyContinue
    }
    if (-not $OwnedProcess.WaitForExit(5000)) { throw "Cannot stop owned build process $($OwnedProcess.Id)" }
}
try {
    # Keep the standard preparation/build entry point. Some Windows sandboxes
    # stall Ninja after its compiler probe exits; detect that specific idle probe
    # and build the same CMake targets with MinGW Makefiles instead.
    $BuildLogDirectory = Join-Path $ProjectRoot 'work/gameplay-build'
    New-Item -ItemType Directory -Force -Path $BuildLogDirectory | Out-Null
    $BuildOut = Join-Path $BuildLogDirectory 'stdout.log'
    $BuildErr = Join-Path $BuildLogDirectory 'stderr.log'
    $BuildDirectory = 'build/host-relwithdebinfo'
    if (-not $SkipBuild) {
        $BuildArguments = '-NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $PSScriptRoot 'build.ps1') + '" -BuildType RelWithDebInfo -Jobs ' + $Jobs
        $BuildProcess = Start-Process -FilePath 'powershell.exe' -ArgumentList $BuildArguments -WorkingDirectory $ProjectRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput $BuildOut -RedirectStandardError $BuildErr
        $null = $BuildProcess.Handle # Retain a handle so Windows PowerShell preserves ExitCode.
        Write-Host "Building AC1; logs: $BuildLogDirectory"
        $UseMake = $false
        while (-not $BuildProcess.WaitForExit(1000)) {
            $LastLine = Get-Content -LiteralPath $BuildOut -Tail 1 -ErrorAction SilentlyContinue
            if ($LastLine -match '^-- Detecting (C|CXX) compiler ABI info$' -and
                ((Get-Date) - (Get-Item -LiteralPath $BuildOut).LastWriteTime).TotalSeconds -ge 30) {
                $UseMake = $true
                Write-Host 'Ninja compiler probe stalled for 30 seconds; using MinGW Makefiles.'
                Stop-OwnedProcessTree $BuildProcess
                break
            }
        }
        $BuildProcess.Refresh()
        if (-not $UseMake -and $BuildProcess.ExitCode -ne 0) {
            Get-Content -LiteralPath $BuildErr -Tail 20 | Write-Host
            throw "Standard AC1 build failed; see $BuildLogDirectory"
        }
        if ($UseMake) {
            $BuildDirectory = 'build/host-make-relwithdebinfo'
            $CMake = Join-Path $env:LOCALAPPDATA 'Android/Sdk/cmake/3.22.1/bin/cmake.exe'
            & $CMake -S . -B $BuildDirectory -G 'MinGW Makefiles' '-DCMAKE_MAKE_PROGRAM=C:/msys64/mingw64/bin/mingw32-make.exe' '-DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe' '-DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe' '-DCMAKE_BUILD_TYPE=RelWithDebInfo'
            if ($LASTEXITCODE -ne 0) { throw 'MinGW Makefiles configure failed' }
            & $CMake --build $BuildDirectory --parallel $Jobs
            if ($LASTEXITCODE -ne 0) { throw 'MinGW Makefiles build failed' }
        }
    }
    $Script = (Resolve-Path -LiteralPath $Script).Path
    $Provider = (Resolve-Path -LiteralPath (Join-Path $ProjectRoot 'work/d3d9_dxvk_probe/d3d9.dll')).Path
    if (-not $OutputDirectory) { $OutputDirectory = Join-Path $ProjectRoot ('work/gameplay/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
    $OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $CaptureDirectory = Join-Path $OutputDirectory 'captures'
    $Values = @{
        AC1_HIDE_FILES = 'videos\UBI_LOGO.bik;videos\Ubisoft_Presents.bik;videos\Warning_Disclaimer.bik;videos\DemoIntro.bik'
        AC1_D3D9_FORCE_WINDOWED = '1'
        AC1_D3D9_LIBRARY = $Provider
        AC1_DISPATCH_LIMIT = '0'
        AC1_INPUT_SCRIPT = $Script
        AC1_D3D9_CAPTURE_DIR = $CaptureDirectory
        AC1_D3D9_CAPTURE_EVERY = '100'
        DXVK_LOG_PATH = $OutputDirectory
    }
    foreach ($Key in $Environment.Keys) { $Values[$Key] = [string]$Environment[$Key] }
    if ($Values.AC1_D3D9_CAPTURE_DIR) { New-Item -ItemType Directory -Force -Path $Values.AC1_D3D9_CAPTURE_DIR | Out-Null }
    foreach ($Key in $Values.Keys) {
        $PreviousEnvironment[$Key] = [Environment]::GetEnvironmentVariable($Key, 'Process')
        [Environment]::SetEnvironmentVariable($Key, $Values[$Key], 'Process')
    }
    $Values.AC1_INPUT_SCRIPT = (Resolve-Path -LiteralPath $Values.AC1_INPUT_SCRIPT).Path
    [Environment]::SetEnvironmentVariable('AC1_INPUT_SCRIPT', $Values.AC1_INPUT_SCRIPT, 'Process')
    Copy-Item -LiteralPath $Values.AC1_INPUT_SCRIPT -Destination (Join-Path $OutputDirectory 'input-script.txt')
    $ScriptHash = (Get-FileHash -LiteralPath $Values.AC1_INPUT_SCRIPT -Algorithm SHA256).Hash.ToLowerInvariant()
    $Executable = Join-Path $ProjectRoot "$BuildDirectory/ac1_startup.exe"
    $ExecutableHash = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
    $Started = [DateTime]::UtcNow
    $Timer = [Diagnostics.Stopwatch]::StartNew()
    $Process = Start-Process -FilePath $Executable -ArgumentList ('"' + $GameExe + '"') -WorkingDirectory $ProjectRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $OutputDirectory 'stdout.log') -RedirectStandardError (Join-Path $OutputDirectory 'startup.log')
    $null = $Process.Handle
    Write-Host "AC1 replay PID=$($Process.Id), script=$Script, evidence=$OutputDirectory"
    $TimedOut = $false
    while (-not $Process.WaitForExit(1000)) {
        if ($Seconds -and $Timer.Elapsed.TotalSeconds -ge $Seconds) {
            $TimedOut = $true
            Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
            $Process.WaitForExit()
            break
        }
    }
    $Process.Refresh()
    [ordered]@{ script=$Values.AC1_INPUT_SCRIPT; script_sha256=$ScriptHash; build_directory=$BuildDirectory; executable_sha256=$ExecutableHash; started_utc=$Started.ToString('o'); elapsed_seconds=$Timer.Elapsed.TotalSeconds; timeout=$TimedOut; exit_code=$Process.ExitCode; captures=$Values.AC1_D3D9_CAPTURE_DIR } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'run.json') -Encoding UTF8
    Write-Host "AC1 replay ended after $([Math]::Round($Timer.Elapsed.TotalSeconds, 2)) s; timeout=$TimedOut; exit=$($Process.ExitCode)"
    if (-not $TimedOut -and $Process.ExitCode -ne 0) { throw "AC1 stopped with exit $($Process.ExitCode); see $OutputDirectory/startup.log" }
} finally {
    Stop-OwnedProcessTree $BuildProcess
    if ($BuildProcess) { $BuildProcess.Dispose() }
    if ($Process) {
        if (-not $Process.HasExited) { Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue; $Process.WaitForExit() }
        $Process.Dispose()
    }
    foreach ($Key in $PreviousEnvironment.Keys) { [Environment]::SetEnvironmentVariable($Key, $PreviousEnvironment[$Key], 'Process') }
    Pop-Location
}

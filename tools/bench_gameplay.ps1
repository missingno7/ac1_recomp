param(
    [ValidateRange(1, 50)][int]$Runs = 3,
    [ValidateRange(60, 86400)][int]$Seconds = 240,
    [switch]$DirectMemory = $true,
    [switch]$TraceEip = $true,
    [string]$LiftArgs = '',
    # Build A/B: AC1_FORCE_INLINE_ACCESSORS (generated-code accessor inlining).
    [switch]$ForceInline = $true,
    # Runtime A/B on the same binary: extra environment and a result label.
    [hashtable]$Env = @{},
    [string]$Label = '',
    # Keep the dedicated build directory (default: delete after copying the binary).
    [switch]$KeepBuild,
    [switch]$AllowDirty,
    [switch]$WaitForQuiet,
    # The hot-function experiments share this PC with other workers. Record the
    # interference and interleave runs instead of requiring a globally idle PC.
    [switch]$AllowConcurrent,
    # Reuse one normal build, copying and stripping it once into the immutable
    # bench store. This avoids a second ~GB build for runtime-only A/B switches.
    [string]$Binary = '',
    [string]$Python = 'D:\Prog\dukemp_recomp\.venv\Scripts\python.exe',
    [string]$GameExe = 'C:\Program Files (x86)\GOG.com\Assassins Creed\AssassinsCreed_Dx9.exe'
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$ownedConfigProc = $null
if (-not ('AC1BenchThreadTimes' -as [type])) { Add-Type -TypeDefinition @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class AC1BenchThreadTimes {
 const uint QUERY_LIMITED=0x0800;
 [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenThread(uint access,bool inherit,uint id);
 [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetThreadTimes(IntPtr h,out long create,out long exit,out long kernel,out long user);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
 public static Dictionary<int,ulong> Snapshot(int[] ids) { var d=new Dictionary<int,ulong>(); foreach(int id in ids){IntPtr h=OpenThread(QUERY_LIMITED,false,(uint)id);if(h==IntPtr.Zero)continue;try{long c,e,k,u;if(GetThreadTimes(h,out c,out e,out k,out u))d[id]=(ulong)(k+u);}finally{CloseHandle(h);}}return d; }
}
"@ }
Push-Location $Root
try {
    function Wait-ForQuietMachine {
        if ($AllowConcurrent) { return }
        while ($true) {
            $busy = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -in @('cc1','cc1plus','ninja','mingw32-make','ac1_startup','python') })
            if (-not $busy.Count) { return }
            $msg = 'Benchmark blocked by concurrent heavy process(es): ' + (($busy | ForEach-Object { "$($_.ProcessName) pid=$($_.Id)" }) -join ', ')
            if (-not $WaitForQuiet) { throw $msg }
            Write-Host "$msg; waiting..."; Start-Sleep -Seconds 5
        }
    }
    & git diff --quiet HEAD --
    if ($LASTEXITCODE -gt 1) { throw 'git diff failed while checking tracked files' }
    $dirty = $LASTEXITCODE -eq 1
    if ($dirty -and -not $AllowDirty) { throw 'Tracked files are modified; commit or pass -AllowDirty. Untracked files do not affect identity.' }
    $sha = (& git rev-parse --short=12 HEAD).Trim()
    $branch = (& git branch --show-current).Trim()
    $dirtyHash = ''
    if ($dirty) { $dirtyHash = (git -c core.safecrlf=false diff --binary HEAD | & $Python -B -c "import sys,hashlib; print(hashlib.sha256(sys.stdin.buffer.read()).hexdigest()[:12])").Trim() }
    $direct = if ($DirectMemory) { 'ON' } else { 'OFF' }
    $trace = if ($TraceEip) { 'ON' } else { 'OFF' }
    $inline = if ($ForceInline) { 'ON' } else { 'OFF' }
    $cfgSpec = "type=RelWithDebInfo;direct=$direct;trace_eip=$trace;force_inline=$inline;lift_args=$LiftArgs;compiler=C:/msys64/mingw64/bin/gcc.exe;generator=Ninja;dirty=$dirtyHash"
    if ($Binary) {
        $Binary = (Resolve-Path -LiteralPath $Binary).Path
        $cfgSpec += ";input_binary_sha256=$((Get-FileHash -LiteralPath $Binary -Algorithm SHA256).Hash.ToLowerInvariant())"
    }
    $cfg = (& $Python -B -c "import hashlib,sys; print(hashlib.sha256(sys.argv[1].encode()).hexdigest()[:12])" $cfgSpec).Trim()
    $key = "$sha-$cfg"
    # One debug-stripped binary per build config; results per runtime label.
    $binDir = Join-Path $Root "work/bench/bin/$key"
    $resultDir = Join-Path $Root ("work/bench/$key" + $(if ($Label) { "-$Label" } else { '' }))
    $binaryPath = Join-Path $binDir 'ac1_startup.exe'
    New-Item -ItemType Directory -Force -Path $binDir | Out-Null
    if ($Binary -and -not (Test-Path $binaryPath)) {
        & 'C:/msys64/mingw64/bin/objcopy.exe' --strip-debug $Binary $binaryPath
        if ($LASTEXITCODE) { throw 'objcopy --strip-debug failed for supplied binary' }
        @{ source=$Binary; config_spec=$cfgSpec; sha256=(Get-FileHash $binaryPath -Algorithm SHA256).Hash.ToLowerInvariant() } |
            ConvertTo-Json | Set-Content (Join-Path $binDir 'binary.json') -Encoding UTF8
    }
    $buildDir = "build/bench-$key"
    $cmake = Join-Path $env:LOCALAPPDATA 'Android/Sdk/cmake/3.22.1/bin/cmake.exe'
    $ninja = Join-Path $env:LOCALAPPDATA 'Android/Sdk/cmake/3.22.1/bin/ninja.exe'
    $generated = 'work/lift_initial'
    if ($LiftArgs) { $generated = "work/lift_$cfg" }
    New-Item -ItemType Directory -Force -Path $resultDir | Out-Null

    function Stop-OwnedTree($OwnedProcess) {
        if (-not $OwnedProcess -or $OwnedProcess.HasExited) { return }
        if (-not ('AC1BenchProcessTree' -as [type])) {
            Add-Type -TypeDefinition @"
using System; using System.Collections.Generic; using System.ComponentModel; using System.Runtime.InteropServices;
public static class AC1BenchProcessTree {
 [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct E { public uint size,usage,pid; public UIntPtr heap; public uint module,threads,parent; public int priority; public uint flags; [MarshalAs(UnmanagedType.ByValTStr,SizeConst=260)] public string name; }
 [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr CreateToolhelp32Snapshot(uint f,uint p);
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] static extern bool Process32FirstW(IntPtr h,ref E e);
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] static extern bool Process32NextW(IntPtr h,ref E e);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
 public static int[] Children(int root) { var rows=new List<KeyValuePair<int,int>>(); var ids=new List<int>{root}; IntPtr h=CreateToolhelp32Snapshot(2,0); if(h==new IntPtr(-1)) throw new Win32Exception(); try { E e=new E(); e.size=(uint)Marshal.SizeOf(typeof(E)); bool more=Process32FirstW(h,ref e); while(more){rows.Add(new KeyValuePair<int,int>((int)e.pid,(int)e.parent));more=Process32NextW(h,ref e);} } finally{CloseHandle(h);} for(int i=0;i<ids.Count;i++) foreach(var r in rows) if(r.Value==ids[i]&&!ids.Contains(r.Key)) ids.Add(r.Key); return ids.ToArray(); }
}
"@
        }
        foreach ($childId in [AC1BenchProcessTree]::Children($OwnedProcess.Id)) { Stop-Process -Id $childId -Force -ErrorAction SilentlyContinue }
        $OwnedProcess.WaitForExit()
    }

    function Test-LiftFresh([string]$Dir) {
        $reportPath = Join-Path $Dir 'lift-report.json'
        if (-not (Test-Path (Join-Path $Dir 'initial.c')) -or -not (Test-Path $reportPath)) { return $false }
        $rep = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
        $configHash = (Get-FileHash config/ac1_dx9.json -Algorithm SHA256).Hash.ToLowerInvariant()
        $driverHash = (Get-FileHash tools/lift_ac1.py -Algorithm SHA256).Hash.ToLowerInvariant()
        $cacheHash = (Get-FileHash tools/emission_cache.py -Algorithm SHA256).Hash.ToLowerInvariant()
        $tablesHash = (Get-FileHash config/startup_initializers.json -Algorithm SHA256).Hash.ToLowerInvariant()
        return $rep.config_sha256 -eq $configHash -and $rep.lift_driver_sha256 -eq $driverHash -and
            $rep.initializer_tables_sha256 -eq $tablesHash -and $rep.generation_input_sha256.emission_cache -eq $cacheHash
    }
    if (-not (Test-Path $binaryPath)) {
        Wait-ForQuietMachine
        if ($LiftArgs -and -not (Test-LiftFresh $generated)) {
            $liftArgv = @(); if ($LiftArgs) { $liftArgv = @($LiftArgs -split '\s+' | Where-Object { $_ }) }
            & $Python -B tools/analyze_pe.py; if ($LASTEXITCODE) { throw 'PE analysis failed' }
            & $Python -B tools/lift_ac1.py --output-dir $generated @liftArgv; if ($LASTEXITCODE) { throw 'AC1 lift failed' }
        } elseif (-not $LiftArgs -and -not (Test-LiftFresh $generated)) {
            & $Python -B tools/analyze_pe.py; if ($LASTEXITCODE) { throw 'PE analysis failed' }
            & $Python -B tools/lift_ac1.py; if ($LASTEXITCODE) { throw 'AC1 lift failed' }
        }
        & $Python -B tools/prepare_runtime.py; if ($LASTEXITCODE) { throw 'Runtime preparation failed' }
        & $Python -B tools/lift_bink.py --if-needed; if ($LASTEXITCODE) { throw 'Bink generation failed' }
        $ninjaExists = Test-Path $ninja
        $makeBuildDir = "build/bench-$key-make"
        $generator = if (Test-Path "$makeBuildDir/CMakeCache.txt") { 'MinGW Makefiles' } elseif ($ninjaExists) { 'Ninja' } else { 'MinGW Makefiles' }
        if ($generator -eq 'MinGW Makefiles') { $buildDir = $makeBuildDir }
        $make = 'C:/msys64/mingw64/bin/mingw32-make.exe'
        $configure = @('-S','.', '-B',$buildDir, '-G',$generator, "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            "-DAC1_DIRECT_MEMORY=$direct", "-DAC1_TRACE_EIP=$trace", "-DAC1_FORCE_INLINE_ACCESSORS=$inline",
            '-DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe', '-DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe',
            "-DAC1_GENERATED_DIR=$((Resolve-Path $generated).Path)")
        if ($generator -eq 'Ninja') { $configure += "-DCMAKE_MAKE_PROGRAM=$ninja" } else { $configure += "-DCMAKE_MAKE_PROGRAM=$make" }
        $cfgLog = Join-Path $resultDir 'configure.log'
        $cfgErr = Join-Path $resultDir 'configure.err'
        $configureLine = ($configure | ForEach-Object { if ([string]$_ -match '\s') { '"' + [string]$_ + '"' } else { [string]$_ } }) -join ' '
        $cfgProc = Start-Process -FilePath $cmake -ArgumentList $configureLine -WorkingDirectory $Root -PassThru -WindowStyle Hidden -RedirectStandardOutput $cfgLog -RedirectStandardError $cfgErr
        $ownedConfigProc = $cfgProc
        $null = $cfgProc.Handle; $useMake = $false
        while (-not $cfgProc.WaitForExit(1000)) {
            $last = Get-Content -LiteralPath $cfgLog -Tail 1 -ErrorAction SilentlyContinue
            if ($generator -eq 'Ninja' -and $last -match '^-- Detecting (C|CXX) compiler ABI info$' -and ((Get-Date)-(Get-Item $cfgLog).LastWriteTime).TotalSeconds -ge 30) {
                Write-Host 'Ninja compiler probe stalled for 30 seconds; retrying this benchmark build with MinGW Makefiles.'
                Stop-OwnedTree $cfgProc; $useMake=$true; break
            }
        }
        $cfgProc.Refresh()
        if ($useMake) {
            $generator='MinGW Makefiles'; $buildDir="build/bench-$key-make"; $make='C:/msys64/mingw64/bin/mingw32-make.exe'
            $configure = @('-S','.', '-B',$buildDir, '-G',$generator, "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DAC1_DIRECT_MEMORY=$direct", "-DAC1_TRACE_EIP=$trace", "-DAC1_FORCE_INLINE_ACCESSORS=$inline", '-DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe', '-DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe', "-DAC1_GENERATED_DIR=$((Resolve-Path $generated).Path)", "-DCMAKE_MAKE_PROGRAM=$make")
            & $cmake @configure; if ($LASTEXITCODE) { throw 'MinGW Makefiles configure failed' }
        } elseif ($cfgProc.ExitCode -ne 0) { Get-Content $cfgErr -Tail 20 | Write-Host; throw 'CMake configure failed' }
        $cache = @{}
        Get-Content "$buildDir/CMakeCache.txt" | ForEach-Object { if ($_ -match '^([^:#]+):[^=]+=([^\r\n]*)$') { $cache[$matches[1]] = $matches[2] } }
        foreach ($pair in @{ CMAKE_BUILD_TYPE='RelWithDebInfo'; AC1_DIRECT_MEMORY=$direct; AC1_TRACE_EIP=$trace; AC1_FORCE_INLINE_ACCESSORS=$inline; AC1_GENERATED_DIR=(Resolve-Path $generated).Path }.GetEnumerator()) {
            $actual = $cache[$pair.Key]; $expected = [string]$pair.Value
            if ($pair.Key -eq 'AC1_GENERATED_DIR') { $actual = $actual -replace '\\','/'; $expected = $expected -replace '\\','/' }
            if ($actual -ne $expected) { throw "CMake cache mismatch for $($pair.Key): requested '$($pair.Value)', found '$($cache[$pair.Key])'" }
        }
        & $cmake --build $buildDir --parallel 16
        if ($LASTEXITCODE) { throw 'Native benchmark build failed' }
        $built = Join-Path $buildDir 'ac1_startup.exe'
        # Debug info is ~1.7 GB; keep the symbol table (nm/profile_report) only.
        & 'C:/msys64/mingw64/bin/objcopy.exe' --strip-debug $built $binaryPath
        if ($LASTEXITCODE) { throw 'objcopy --strip-debug failed' }
        $binaryHash = (Get-FileHash $binaryPath -Algorithm SHA256).Hash.ToLowerInvariant()
        $codeBytes = (Get-Item $binaryPath).Length
        @{ sha256=$binaryHash; build_dir=$buildDir; config_spec=$cfgSpec; stripped_bytes=$codeBytes } | ConvertTo-Json | Set-Content (Join-Path $binDir 'binary.json') -Encoding UTF8
        if (-not $KeepBuild) { Remove-Item -LiteralPath $buildDir -Recurse -Force }
    }
    $binaryHash = (Get-FileHash $binaryPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $cpuModel = (Get-ItemProperty 'HKLM:\HARDWARE\DESCRIPTION\System\CentralProcessor\0').ProcessorNameString
    if (-not ('AC1BenchMemory' -as [type])) { Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class AC1BenchMemory { [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Auto)] public class M { public uint Length=(uint)Marshal.SizeOf(typeof(M)); public uint Load; public ulong TotalPhys,AvailPhys,TotalPageFile,AvailPageFile,TotalVirtual,AvailVirtual; public ulong AvailExtended; } [DllImport("kernel32.dll",SetLastError=true)] public static extern bool GlobalMemoryStatusEx([In,Out] M m); }
"@ }
    $mem = New-Object AC1BenchMemory+M; if (-not [AC1BenchMemory]::GlobalMemoryStatusEx($mem)) { throw 'Cannot read total physical memory' }
    $machine = [ordered]@{ cpu_model=$cpuModel; logical_cores=[Environment]::ProcessorCount; physical_memory_bytes=[long]$mem.TotalPhys }
    $gpuAvailable = $null -ne (Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue)
    $runsOut = @()
    for ($i=1; $i -le $Runs; $i++) {
        Wait-ForQuietMachine
        $runDir = Join-Path $resultDir ("run-{0:D2}" -f $i)
        New-Item -ItemType Directory -Force -Path $runDir | Out-Null
        $envs = @{
            AC1_HIDE_FILES='videos\UBI_LOGO.bik;videos\Ubisoft_Presents.bik;videos\Warning_Disclaimer.bik;videos\DemoIntro.bik'
            AC1_D3D9_FORCE_WINDOWED='1'; AC1_D3D9_LIBRARY=(Resolve-Path work/d3d9_dxvk_probe/d3d9.dll).Path
            AC1_DISPATCH_LIMIT='0'; AC1_INPUT_SCRIPT=(Resolve-Path tools/input/to_gameplay.txt).Path
            AC1_D3D9_CAPTURE_DIR=(Join-Path $runDir 'captures')
            AC1_D3D9_TRACE_FRAME='1'; AC1_PROFILE_FRAMES='1'; AC1_PROFILE='0'; AC1_D3D9_CAPTURE_EVERY=''; DXVK_LOG_PATH=$runDir
        }
        foreach ($k in $Env.Keys) { $envs[$k] = [string]$Env[$k] }
        $envs['AC1_PROFILE_DIR'] = (Join-Path $runDir 'profile')
        $profileDir = Join-Path $runDir 'profile'
        $profileSource = $profileDir
        New-Item -ItemType Directory -Force -Path $envs.AC1_D3D9_CAPTURE_DIR,$profileDir,$profileSource | Out-Null
        $profileBefore = @{}; Get-ChildItem "$profileSource/raw_*.json" -ErrorAction SilentlyContinue | ForEach-Object { $profileBefore[$_.Name] = $_.LastWriteTimeUtc }
        $gpuLog = Join-Path $runDir 'gpu.csv'; $gpuProc = $null
        $proc = $null; $timer = [Diagnostics.Stopwatch]::StartNew(); $gameTimer = $null; $cpuAtStart = 0.0; $topThreads = @(); $threadStart = @{}; $durationReached = $false; $runStart = [DateTime]::UtcNow
        $previousEnv = @{}; foreach ($k in $envs.Keys) { $previousEnv[$k] = [Environment]::GetEnvironmentVariable($k,'Process'); [Environment]::SetEnvironmentVariable($k,[string]$envs[$k],'Process') }
        try {
            $proc = Start-Process -FilePath $binaryPath -ArgumentList ('"' + $GameExe + '"') -WorkingDirectory $Root -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $runDir 'stdout.log') -RedirectStandardError (Join-Path $runDir 'startup.log')
            $null = $proc.Handle
            while (-not $proc.WaitForExit(1000)) {
                if (-not $gameTimer) {
                    $nframes = 0
                    Get-ChildItem "$profileSource/raw_*.json" -ErrorAction SilentlyContinue | Where-Object { -not $profileBefore.ContainsKey($_.Name) -or $_.LastWriteTimeUtc -gt $profileBefore[$_.Name] } | ForEach-Object { try { $nframes += (@((Get-Content $_.FullName -Raw | ConvertFrom-Json).frames)).Count } catch {} }
                    if ($nframes -ge 1628) {
                        $gameTimer = [Diagnostics.Stopwatch]::StartNew(); $cpuAtStart = $proc.CPU
                        $threadIds = @($proc.Threads | ForEach-Object { [int]$_.Id }); $threadStart = [AC1BenchThreadTimes]::Snapshot([int[]]$threadIds)
                        if ($gpuAvailable) { $gpuProc = Start-Process nvidia-smi.exe -ArgumentList @('--query-gpu=utilization.gpu,clocks.gr','--format=csv,noheader,nounits','-lms','500') -PassThru -WindowStyle Hidden -RedirectStandardOutput $gpuLog -RedirectStandardError (Join-Path $runDir 'gpu.err') }
                        Write-Host "Run $i gameplay measurement started at approximately frame 1628."
                    }
                }
                if ($gameTimer -and $gameTimer.Elapsed.TotalSeconds -ge $Seconds) { $durationReached = $true; break }
                if (-not $gameTimer -and $timer.Elapsed.TotalSeconds -ge ($Seconds + 180)) { break }
            }
            $proc.Refresh(); $cpuNow = $proc.CPU
            try {
                $threadIds = @($proc.Threads | ForEach-Object { [int]$_.Id }); $threadEnd = [AC1BenchThreadTimes]::Snapshot([int[]]$threadIds)
                $topThreads = @($threadEnd.Keys | ForEach-Object { $tid=$_; $before=if($threadStart.ContainsKey($tid)){$threadStart[$tid]}else{$threadEnd[$tid]}; [ordered]@{ tid=$tid; cpu_seconds=([Math]::Max([long]0,[long]$threadEnd[$tid]-[long]$before)/10000000.0) } } | Sort-Object cpu_seconds -Descending | Select-Object -First 10)
            } catch { $topThreads = @() }
            if ($gameTimer) { $cpuNow = $proc.CPU }
            $proc.Refresh()
        } finally {
            if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue; $proc.WaitForExit() }
            if ($gpuProc -and -not $gpuProc.HasExited) { Stop-Process -Id $gpuProc.Id -Force -ErrorAction SilentlyContinue; $gpuProc.WaitForExit() }
            if ($profileSource -ne $profileDir) {
                Get-ChildItem "$profileSource/raw_*.json" -ErrorAction SilentlyContinue | Where-Object { -not $profileBefore.ContainsKey($_.Name) -or $_.LastWriteTimeUtc -gt $profileBefore[$_.Name] } | ForEach-Object { Copy-Item $_.FullName $profileDir; $bin = Join-Path $_.DirectoryName ($_.BaseName + '.bin'); if (Test-Path $bin) { Copy-Item $bin $profileDir } }
            }
            foreach ($k in $envs.Keys) { [Environment]::SetEnvironmentVariable($k,$previousEnv[$k],'Process') }
        }
        $runOut = & $Python -B tools/bench_report.py --internal-run $runDir --seconds $Seconds | ConvertFrom-Json
        $cpuSeconds = [Math]::Max(0, $cpuNow - $cpuAtStart)
        $measurementSeconds = if ($gameTimer) { [Math]::Min($Seconds,$gameTimer.Elapsed.TotalSeconds) } else { 0 }
        $runOut | Add-Member -NotePropertyName process_cpu_seconds -NotePropertyValue $cpuSeconds -Force
        $runOut | Add-Member -NotePropertyName cpu_utilization_cores -NotePropertyValue $(if ($measurementSeconds) { $cpuSeconds/$measurementSeconds } else { $null }) -Force
        $runOut | Add-Member -NotePropertyName top_threads -NotePropertyValue $topThreads -Force
        $runOut | Add-Member -NotePropertyName run -NotePropertyValue $i -Force
        $runOut | Add-Member -NotePropertyName elapsed_seconds -NotePropertyValue $timer.Elapsed.TotalSeconds -Force
        $runOut | Add-Member -NotePropertyName exit_code -NotePropertyValue $proc.ExitCode -Force
        $runOut | Add-Member -NotePropertyName timed_out -NotePropertyValue $durationReached -Force
        $runOut | Add-Member -NotePropertyName evidence -NotePropertyValue $runDir -Force
        $runsOut += $runOut
    }
    $actualBuildDir = if (Test-Path "build/bench-$key-make/CMakeCache.txt") { "build/bench-$key-make" } else { $buildDir }
    if ($Binary) { $actualBuildDir = Split-Path -Parent $Binary }
    $actualGenerator = 'unknown'
    if (Test-Path "$actualBuildDir/CMakeCache.txt") { $generatorLine = Select-String -Path "$actualBuildDir/CMakeCache.txt" -Pattern '^CMAKE_GENERATOR:INTERNAL=' | Select-Object -First 1; if ($generatorLine) { $actualGenerator = ($generatorLine.Line -split '=',2)[1] } }
    $obj = [ordered]@{ identity=[ordered]@{ sha=$sha; branch=$branch; dirty=$dirty; dirty_diff_sha256=$dirtyHash; config_id=$cfg; config=$cfgSpec; binary_sha256=$binaryHash; label=$Label; runtime_env=$Env }; machine=$machine; environment=[ordered]@{ seconds=$Seconds; requested_runs=$Runs; replay='tools/input/to_gameplay.txt'; profile='frames-only'; profiler='none'; actual_build_generator=$actualGenerator; gpu_sampling=if ($gpuAvailable) {'nvidia-smi 500ms, measured window'} else {'unavailable'} }; runs=$runsOut }
    $obj | ConvertTo-Json -Depth 20 | Set-Content (Join-Path $resultDir 'result.json') -Encoding UTF8
    & $Python -B tools/bench_report.py --summary $resultDir
    Write-Host "Benchmark evidence: $resultDir"
} finally { if ($ownedConfigProc -and -not $ownedConfigProc.HasExited) { Stop-OwnedTree $ownedConfigProc }; Pop-Location }

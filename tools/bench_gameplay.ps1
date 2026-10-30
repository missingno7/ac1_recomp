param(
    [ValidateRange(1, 50)][int]$Runs = 3,
    [ValidateRange(1, 86400)][int]$Seconds = 180,
    [ValidateRange(1, 3600)][int]$StartupTimeoutSeconds = 120,
    [ValidateRange(1, 1000000)][int]$StartPresent = 1800,
    [ValidateRange(1, 1000000)][int]$EndPresent = 2200,
    [ValidateRange(1, 4096)][int]$FrameStatsFlushFrames = 8,
    [ValidateRange(3, 8)][int]$MinPairs = 3,
    [ValidateRange(3, 8)][int]$MaxPairs = 8,
    [ValidateRange(0.1, 50.0)][double]$PracticalPercent = 1.0,
    [ValidateSet('gameplay','menu')][string]$Workload = 'gameplay',
    [switch]$DirectMemory = $true,
    [switch]$TraceEip = $true,
    [string]$LiftArgs = '',
    # Build A/B: AC1_FORCE_INLINE_ACCESSORS (generated-code accessor inlining).
    [switch]$ForceInline = $true,
    # Runtime A/B on the same binary: extra environment and a result label.
    [hashtable]$Env = @{},
    [string]$EnvJson = '',
    [string]$Binary = '',
    [string]$BinaryB = '',
    [switch]$Compare,
    [switch]$Exploratory,
    [hashtable]$EnvB = @{},
    [string]$EnvBJson = '',
    [string]$DirectMemoryB = '',
    [string]$TraceEipB = '',
    [string]$ForceInlineB = '',
    [string]$LiftArgsB = '',
    [string]$ResultDirOverride = '',
    [string]$ComparisonDir = '',
    # Optional controlled private save/profile state, restored before each side.
    [string]$UserdataSeed = '',
    [string]$Label = '',
    # Keep the dedicated build directory (default: delete after copying the binary).
    [switch]$KeepBuild,
    [switch]$AllowDirty,
    [switch]$WaitForQuiet,
    [switch]$AllowBusy,
    [string]$Python = 'D:\Prog\dukemp_recomp\.venv\Scripts\python.exe',
    [string]$GameExe = 'C:\Program Files (x86)\GOG.com\Assassins Creed\AssassinsCreed_Dx9.exe'
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
if ($EnvJson) {
    $parsedEnv = ConvertFrom-Json -InputObject $EnvJson
    foreach ($item in $parsedEnv.PSObject.Properties) { $Env[$item.Name] = [string]$item.Value }
}
if ($EnvBJson) {
    $parsedEnvB = ConvertFrom-Json -InputObject $EnvBJson
    foreach ($item in $parsedEnvB.PSObject.Properties) { $EnvB[$item.Name] = [string]$item.Value }
}
$ownedConfigProc = $null
$benchLockStream = $null
$benchLockPath = 'D:\ac1_bench.lock'
Push-Location $Root
try {
    function Wait-ForQuietMachine {
        if ($AllowBusy) { return }
        while ($true) {
            $busy = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -in @('cc1','cc1plus','ninja','mingw32-make','ac1_startup','python') })
            if (-not $busy.Count) { return }
            $msg = 'Benchmark blocked by concurrent heavy process(es): ' + (($busy | ForEach-Object { "$($_.ProcessName) pid=$($_.Id)" }) -join ', ')
            if (-not $WaitForQuiet) { throw $msg }
            Write-Host "$msg; waiting..."; Start-Sleep -Seconds 5
        }
    }
    & git -c core.safecrlf=false diff --quiet HEAD --
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
    $cfg = (& $Python -B -c "import hashlib,sys; print(hashlib.sha256(sys.argv[1].encode()).hexdigest()[:12])" $cfgSpec).Trim()
    $key = "$sha-$cfg"
    # One debug-stripped binary per build config; results per runtime label.
    $binDir = Join-Path $Root "work/bench/bin/$key"
    $resultDir = Join-Path $Root ("work/bench/$key" + $(if ($Label) { "-$Label" } else { '' }))
    $binaryPath = if ($Binary) { (Resolve-Path -LiteralPath $Binary).Path } else { Join-Path $binDir 'ac1_startup.exe' }
    New-Item -ItemType Directory -Force -Path $binDir | Out-Null
    $buildDir = "build/bench-$key"
    $cmake = Join-Path $env:LOCALAPPDATA 'Android/Sdk/cmake/3.22.1/bin/cmake.exe'
    $ninja = Join-Path $env:LOCALAPPDATA 'Android/Sdk/cmake/3.22.1/bin/ninja.exe'
    $generated = 'work/lift_initial'
    if ($LiftArgs) { $generated = "work/lift_$cfg" }
    if ($ResultDirOverride) { $resultDir = if ([IO.Path]::IsPathRooted($ResultDirOverride)) { $ResultDirOverride } else { Join-Path $Root $ResultDirOverride } }
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
    if ($BinaryB -or $Compare) {
        if (-not $Exploratory) {
            try { $benchLockStream = [IO.File]::Open($benchLockPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None) }
            catch { throw "Controlled comparison refused: $benchLockPath already exists or cannot be created. Use -Exploratory to run without the lock." }
            $lockText = "worker=$env:USERNAME`nstarted=$([DateTime]::UtcNow.ToString('o'))`npurpose=AC1 fixed-present A/B benchmark`n"
            $lockBytes = [Text.Encoding]::UTF8.GetBytes($lockText); $benchLockStream.Write($lockBytes,0,$lockBytes.Length); $benchLockStream.Flush()
        }
        if ($MaxPairs -lt $MinPairs) { throw '-MaxPairs must be greater than or equal to -MinPairs.' }
        $comparisonId = 'comparison-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,6)
        $comparisonDir = if ($ComparisonDir) { if ([IO.Path]::IsPathRooted($ComparisonDir)) { $ComparisonDir } else { Join-Path $Root $ComparisonDir } } else { Join-Path $Root "work/bench/$comparisonId" }
        New-Item -ItemType Directory -Force -Path $comparisonDir | Out-Null
        $sideRuns = @{ A=@(); B=@() }
        $sideLabels = @{ A=$(if ($Label) { $Label } else { 'A' }); B='B' }
        $decision = $null
        $decisionPath = Join-Path $comparisonDir 'adaptive-decision-input.json'
        for ($pair=1; $pair -le $MaxPairs; $pair++) {
            $pairOrder = if (($pair % 2) -eq 1) { @('A','B') } else { @('B','A') }
            foreach ($side in $pairOrder) {
                $sideBinary = if ($side -eq 'A') { $Binary } else { $BinaryB }
                $sideEnv = if ($side -eq 'A') { $Env } else { $EnvB }
                $sideDir = Join-Path $comparisonDir ("{0}/pair-{1:D2}" -f $side,$pair)
                New-Item -ItemType Directory -Force -Path $sideDir | Out-Null
                if ($UserdataSeed) {
                    & $Python -B tools/bench_userdata.py --seed $UserdataSeed --archive (Join-Path $sideDir 'userdata-before') --manifest (Join-Path $sideDir 'userdata-input.json')
                    if ($LASTEXITCODE -ne 0) { throw 'Private userdata seed restoration failed' }
                }
                $childArgs = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$PSCommandPath,
                    '-Runs','1','-Seconds',[string]$Seconds,'-StartupTimeoutSeconds',[string]$StartupTimeoutSeconds,
                    '-StartPresent',[string]$StartPresent,'-EndPresent',[string]$EndPresent,'-FrameStatsFlushFrames',[string]$FrameStatsFlushFrames,'-Workload',$Workload,
                    '-ResultDirOverride',$sideDir,'-Label',"${side}-pair-${pair}",'-Python',$Python,'-GameExe',$GameExe,'-AllowDirty')
                if ($sideBinary) { $childArgs += @('-Binary',$sideBinary) }
                $childDirect = $DirectMemory; $childTrace = $TraceEip; $childInline = $ForceInline; $childLift = $LiftArgs
                if ($side -eq 'B') {
                    if ($DirectMemoryB -ne '') { $childDirect = $DirectMemoryB -eq 'ON' -or $DirectMemoryB -eq 'true' }
                    if ($TraceEipB -ne '') { $childTrace = $TraceEipB -eq 'ON' -or $TraceEipB -eq 'true' }
                    if ($ForceInlineB -ne '') { $childInline = $ForceInlineB -eq 'ON' -or $ForceInlineB -eq 'true' }
                    if ($LiftArgsB -ne '') { $childLift = $LiftArgsB }
                }
                $childArgs += $(if ($childDirect) { '-DirectMemory' } else { '-DirectMemory:$false' })
                $childArgs += $(if ($childTrace) { '-TraceEip' } else { '-TraceEip:$false' })
                $childArgs += $(if ($childInline) { '-ForceInline' } else { '-ForceInline:$false' })
                if ($childLift) { $childArgs += @('-LiftArgs',$childLift) }
                if ($sideEnv.Count) { $childArgs += @('-EnvJson',($sideEnv | ConvertTo-Json -Compress)) }
                if ($WaitForQuiet) { $childArgs += '-WaitForQuiet' }
                if ($AllowBusy) { $childArgs += '-AllowBusy' }
                & powershell.exe @childArgs
                if ($LASTEXITCODE -ne 0) { throw "Interleaved $side pair $pair failed to execute (exit $LASTEXITCODE); see $sideDir" }
                $one = Get-Content -LiteralPath (Join-Path $sideDir 'result.json') -Raw | ConvertFrom-Json
                $sideRuns[$side] += $one.runs[0]
                $sideLabels[$side] = if ($one.identity.label) { $one.identity.label } elseif ($sideBinary) { (Split-Path $sideBinary -Leaf) } else { $one.identity.config_id }
            }
            $pairedInputs = @(); $baselineInputs = @(); $pairEquivalence = @()
            for ($pairIndex=0; $pairIndex -lt $sideRuns.A.Count; $pairIndex++) {
                $aRun = $sideRuns.A[$pairIndex]; $bRun = $sideRuns.B[$pairIndex]
                $sameCommands = $null -ne $aRun.input_command_index_start -and
                    $aRun.input_command_index_start -eq $bRun.input_command_index_start -and
                    $aRun.input_command_index_end -eq $bRun.input_command_index_end
                $guestA = [double]$aRun.guest_time_elapsed_ns; $guestB = [double]$bRun.guest_time_elapsed_ns
                $guestComparable = $guestA -gt 0 -and $guestB -gt 0 -and ([Math]::Abs($guestA-$guestB) / [Math]::Max($guestA,$guestB)) -le 0.01
                $capturesPresent = @($aRun.boundary_captures).Count -eq 2 -and @($bRun.boundary_captures).Count -eq 2
                $observableResultsMatch = $capturesPresent -and (($aRun.boundary_capture_sha256 | ConvertTo-Json -Compress) -eq ($bRun.boundary_capture_sha256 | ConvertTo-Json -Compress))
                $equivalent = $aRun.reached_workload -and $bRun.reached_workload -and $sameCommands -and $guestComparable -and $observableResultsMatch
                $pairEquivalence += [ordered]@{ pair=$pairIndex+1; equivalent=$equivalent; command_indices_match=$sameCommands; guest_time_comparable=$guestComparable; guest_time_A_ns=$aRun.guest_time_elapsed_ns; guest_time_B_ns=$bRun.guest_time_elapsed_ns; observable_boundary_hashes_match=$observableResultsMatch; captures_A=$aRun.boundary_captures; captures_B=$bRun.boundary_captures }
                if ($equivalent -and $null -ne $aRun.main_thread_cpu_ms_per_frame -and $null -ne $bRun.main_thread_cpu_ms_per_frame) {
                    $pairedInputs += [double]$bRun.main_thread_cpu_ms_per_frame - [double]$aRun.main_thread_cpu_ms_per_frame
                    $baselineInputs += [double]$aRun.main_thread_cpu_ms_per_frame
                }
            }
            [ordered]@{ executed_pairs=$pair; deltas=$pairedInputs; baseline=$baselineInputs } |
                ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $decisionPath -Encoding UTF8
            $decision = (& $Python -B tools/bench_report.py --decision-json $decisionPath `
                --min-pairs $MinPairs --max-pairs $MaxPairs --practical-percent $PracticalPercent | ConvertFrom-Json)
            Write-Host "Pair $pair/$MaxPairs ($($pairOrder -join '/')): equivalent=$($pairEquivalence[-1].equivalent), valid=$($decision.valid_pairs), paired main-CPU delta=$($decision.paired_delta_median_ms) ms/frame, verdict=$($decision.verdict)."
            if ($decision.stop) {
                Write-Host "Adaptive comparison stopped: $($decision.stop_reason)"
                break
            }
        }
        $comparison = [ordered]@{
            comparison=$true; exploratory=[bool]$Exploratory; lock_acquired=([bool]$benchLockStream); pin_mask='0x0000FFFF'; pair_count=$pair; min_pairs=$MinPairs; max_pairs=$MaxPairs; safety_timeout_seconds=$Seconds
            start_present=$StartPresent; end_present=$EndPresent; workload_definition='fixed successful Present range; gameplay uses tools/input/to_gameplay.txt'; pair_equivalence=$pairEquivalence
            practical_percent=$PracticalPercent; adaptive_decision=$decision; host_load_override=[bool]$AllowBusy
            userdata_seed=$UserdataSeed; workload=$Workload; vsync='off (DXVK d3d9.presentInterval=0)'
            A=[ordered]@{ label=$sideLabels.A; binary=$Binary; runtime_env=$Env; runs=$sideRuns.A; failures=@($sideRuns.A | Where-Object { -not $_.reached_workload }).Count }
            B=[ordered]@{ label=$sideLabels.B; binary=$BinaryB; runtime_env=$EnvB; runs=$sideRuns.B; failures=@($sideRuns.B | Where-Object { -not $_.reached_workload }).Count }
            paired_deltas=@{}
        }
        $comparison | ConvertTo-Json -Depth 30 | Set-Content (Join-Path $comparisonDir 'result.json') -Encoding UTF8
        & $Python -B tools/bench_report.py --summary $comparisonDir
        Write-Host "Interleaved comparison evidence: $comparisonDir"
        return
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
    if (-not ('AC1BenchTopology' -as [type])) { Add-Type -TypeDefinition @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class AC1BenchTopology {
 [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetLogicalProcessorInformationEx(int relation, byte[] buffer, ref uint length);
 [DllImport("powrprof.dll")] static extern int CallNtPowerInformation(int level, IntPtr input, uint inputLength, IntPtr output, uint outputLength);
 public static ulong[] L3Masks() { uint n=0; GetLogicalProcessorInformationEx(2,null,ref n); byte[] b=new byte[n]; if(!GetLogicalProcessorInformationEx(2,b,ref n)) throw new System.ComponentModel.Win32Exception(); var a=new List<ulong>(); int o=0; while(o<n) { int size=BitConverter.ToInt32(b,o+4); if(BitConverter.ToInt32(b,o)==2 && b[o+8]==3) { int groups=BitConverter.ToUInt16(b,o+38); for(int g=0;g<groups;g++) { int q=o+40+g*16; if(BitConverter.ToUInt16(b,q+8)==0) a.Add(BitConverter.ToUInt64(b,q)); } } o+=size; } return a.ToArray(); }
 public static double CurrentMhz(ulong mask) { int count=Environment.ProcessorCount; int bytes=count*24; IntPtr p=Marshal.AllocHGlobal(bytes); try { if(CallNtPowerInformation(11,IntPtr.Zero,0,p,(uint)bytes)!=0) return -1; double sum=0; int n=0; for(int i=0;i<count&&i<64;i++) if((mask&(1UL<<i))!=0) { sum+=Marshal.ReadInt32(p,i*24+8); n++; } return n==0?-1:sum/n; } finally { Marshal.FreeHGlobal(p); } }
}
"@ }
    $pinMask = [UInt64]65535
    $l3Masks = [AC1BenchTopology]::L3Masks()
    if (-not ($l3Masks -contains $pinMask)) { throw 'CCD0 affinity mask 0x0000FFFF does not match a queried L3 cache domain.' }
    $l3DomainMasks = @($l3Masks | ForEach-Object { '0x{0:X8}' -f $_ })
    if (-not ('AC1BenchMemory' -as [type])) { Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class AC1BenchMemory { [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Auto)] public class M { public uint Length=(uint)Marshal.SizeOf(typeof(M)); public uint Load; public ulong TotalPhys,AvailPhys,TotalPageFile,AvailPageFile,TotalVirtual,AvailVirtual; public ulong AvailExtended; } [DllImport("kernel32.dll",SetLastError=true)] public static extern bool GlobalMemoryStatusEx([In,Out] M m); }
"@ }
    $mem = New-Object AC1BenchMemory+M; if (-not [AC1BenchMemory]::GlobalMemoryStatusEx($mem)) { throw 'Cannot read total physical memory' }
    $machine = [ordered]@{ cpu_model=$cpuModel; logical_cores=[Environment]::ProcessorCount; physical_memory_bytes=[long]$mem.TotalPhys }
    $gpuTool = Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $gpuTool) {
        foreach ($candidate in @('C:\Windows\System32\nvidia-smi.exe','C:\Program Files\NVIDIA Corporation\NVSMI\nvidia-smi.exe')) {
            if (Test-Path -LiteralPath $candidate) { $gpuTool = [pscustomobject]@{ Source=$candidate }; break }
        }
    }
    $gpuAvailable = $null -ne $gpuTool
    $runsOut = @()
    for ($i=1; $i -le $Runs; $i++) {
        Wait-ForQuietMachine
        $runDir = Join-Path $resultDir ("run-{0:D2}" -f $i)
        New-Item -ItemType Directory -Force -Path $runDir | Out-Null
        $dxvkConfig = Join-Path $runDir 'dxvk.conf'
        Set-Content -LiteralPath $dxvkConfig -Value 'd3d9.presentInterval = 0' -Encoding ASCII
        $envs = @{
            AC1_HIDE_FILES='videos\UBI_LOGO.bik;videos\Ubisoft_Presents.bik;videos\Warning_Disclaimer.bik;videos\DemoIntro.bik'
            AC1_D3D9_FORCE_WINDOWED='1'; AC1_D3D9_LIBRARY=(Resolve-Path work/d3d9_dxvk_probe/d3d9.dll).Path
            AC1_DISPATCH_LIMIT='0'; AC1_AUDIO_MUTE='1'
            AC1_D3D9_CAPTURE_DIR=(Join-Path $runDir 'captures')
            AC1_D3D9_TRACE_FRAME='1'; AC1_PROFILE_FRAMES='0'; AC1_PROFILE='0'; AC1_D3D9_CAPTURE_EVERY=''; DXVK_LOG_PATH=$runDir
            AC1_FRAME_STATS=(Join-Path $runDir 'frame-stats.csv'); AC1_FRAME_STATS_FLUSH_FRAMES=[string]$FrameStatsFlushFrames; AC1_FRAME_STATS_END_PRESENT=[string]$EndPresent; DXVK_CONFIG_FILE=$dxvkConfig
        }
        $envs['AC1_INPUT_SCRIPT'] = if ($Workload -eq 'gameplay') { (Resolve-Path tools/input/to_gameplay.txt).Path } else { '' }
        foreach ($k in $Env.Keys) { $envs[$k] = [string]$Env[$k] }
        # Benchmark invariants stay enabled even when caller-supplied runtime
        # environment overrides are used for A/B experiments.
        $envs['AC1_FRAME_STATS'] = Join-Path $runDir 'frame-stats.csv'
        $envs['AC1_FRAME_STATS_FLUSH_FRAMES'] = [string]$FrameStatsFlushFrames
        $envs['AC1_FRAME_STATS_END_PRESENT'] = [string]$EndPresent
        $envs['AC1_PROFILE_FRAMES'] = '0'
        $envs['AC1_D3D9_CAPTURE_DIR'] = Join-Path $runDir 'captures'
        $envs['AC1_D3D9_CAPTURE_POINTS'] = "$StartPresent,$EndPresent"
        $envs['AC1_D3D9_CAPTURE_EVERY'] = ''
        $envs['AC1_D3D9_TRACE_FRAME'] = '1'
        $envs['AC1_INPUT_SCRIPT'] = if ($Workload -eq 'gameplay') { (Resolve-Path tools/input/to_gameplay.txt).Path } else { '' }
        $envs['DXVK_CONFIG_FILE'] = $dxvkConfig
        $envs['DXVK_CONFIG'] = 'd3d9.presentInterval = 0'
        $envs['AC1_PROFILE_DIR'] = (Join-Path $runDir 'profile')
        $envs['AC1_D3D9_CAPTURE_EVERY'] = ''
        $envs['AC1_D3D9_CAPTURE_POINTS'] = "$StartPresent,$EndPresent"
        $profileDir = Join-Path $runDir 'profile'
        $profileSource = Join-Path $Root 'work/work/profile'
        New-Item -ItemType Directory -Force -Path $envs.AC1_D3D9_CAPTURE_DIR,$profileDir,$profileSource | Out-Null
        $profileBefore = @{}; Get-ChildItem "$profileSource/raw_*.json" -ErrorAction SilentlyContinue | ForEach-Object { $profileBefore[$_.Name] = $_.LastWriteTimeUtc }
        $gpuLog = Join-Path $runDir 'gpu.csv'; $gpuProc = $null
        $proc = $null; $timer = [Diagnostics.Stopwatch]::StartNew(); $gameTimer = $null; $durationReached = $false; $safetyTimeoutReached = $false
        $frequencyStartMhz = $null; $frequencyEndMhz = $null
        $otherCpuStart = @{}; $hostWindowClock = $null; $otherCpuSeconds = $null; $otherCpuPercent = $null
        $measurementPrefixMs = 0.0
        $cpuSampleWindowFrames = 0
        $sceneStartFrame = $StartPresent
        $measurementStartFrame = $sceneStartFrame
        $previousEnv = @{}; foreach ($k in $envs.Keys) { $previousEnv[$k] = [Environment]::GetEnvironmentVariable($k,'Process'); [Environment]::SetEnvironmentVariable($k,[string]$envs[$k],'Process') }
        try {
            $proc = Start-Process -FilePath $binaryPath -ArgumentList ('"' + $GameExe + '"') -WorkingDirectory $Root -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $runDir 'stdout.log') -RedirectStandardError (Join-Path $runDir 'startup.log')
            $null = $proc.Handle
            $pinMask = [long]65535
            $proc.ProcessorAffinity = [IntPtr]$pinMask
            $affinityReadback = [long]$proc.ProcessorAffinity
            if ($affinityReadback -ne $pinMask) { throw 'Failed to pin game process to CCD0 mask 0x0000FFFF.' }
            while (-not $proc.WaitForExit(50)) {
                $presentNow = 0
                $statsPath = Join-Path $runDir 'frame-stats.csv'
                if (Test-Path -LiteralPath $statsPath) {
                    try { $lastStatsLine = Get-Content -LiteralPath $statsPath -Tail 1 -ErrorAction Stop; if ($lastStatsLine -match '^(\d+),') { $presentNow = [int]$Matches[1] } } catch { }
                }
                if ($null -eq $frequencyStartMhz -and $presentNow -ge $StartPresent) {
                    $frequencyStartMhz = [AC1BenchTopology]::CurrentMhz($pinMask)
                    $hostWindowClock = [Diagnostics.Stopwatch]::StartNew()
                    foreach ($p in Get-Process -ErrorAction SilentlyContinue) { if ($p.Id -ne $proc.Id) { try { $otherCpuStart[$p.Id] = $p.TotalProcessorTime.TotalSeconds } catch { } } }
                }
                if ($presentNow -ge $EndPresent) { $durationReached = $true; break }
                if ($timer.Elapsed.TotalSeconds -ge ($Seconds + $StartupTimeoutSeconds)) { $safetyTimeoutReached = $true; break }
            }
            $proc.Refresh()
            if ($durationReached -and -not $proc.HasExited) {
                # Freeze accounting and stop the game as soon as the measured
                # runtime frame-stat window ends.
                Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
                $proc.WaitForExit()
                if ($gpuProc -and -not $gpuProc.HasExited) {
                    Stop-Process -Id $gpuProc.Id -Force -ErrorAction SilentlyContinue
                    $gpuProc.WaitForExit()
                }
            }
        } finally {
            if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue; $proc.WaitForExit() }
            if ($gpuProc -and -not $gpuProc.HasExited) { Stop-Process -Id $gpuProc.Id -Force -ErrorAction SilentlyContinue; $gpuProc.WaitForExit() }
            Get-ChildItem "$profileSource/raw_*.json" -ErrorAction SilentlyContinue | Where-Object { -not $profileBefore.ContainsKey($_.Name) -or $_.LastWriteTimeUtc -gt $profileBefore[$_.Name] } | ForEach-Object { Copy-Item $_.FullName $profileDir; $bin = Join-Path $_.DirectoryName ($_.BaseName + '.bin'); if (Test-Path $bin) { Copy-Item $bin $profileDir } }
            foreach ($k in $envs.Keys) { [Environment]::SetEnvironmentVariable($k,$previousEnv[$k],'Process') }
        }
        $runOut = & $Python -B tools/bench_report.py --internal-run $runDir --seconds $Seconds --workload $Workload `
            --start-present $measurementStartFrame --end-present $EndPresent --scene-present $sceneStartFrame --flush-frames $FrameStatsFlushFrames | ConvertFrom-Json
        $runOut | Add-Member -NotePropertyName run -NotePropertyValue $i -Force
        $runOut | Add-Member -NotePropertyName measurement_start_present_index -NotePropertyValue $measurementStartFrame -Force
        $runOut | Add-Member -NotePropertyName scene_start_present_index -NotePropertyValue $sceneStartFrame -Force
        $runOut | Add-Member -NotePropertyName measurement_start_delay_ms -NotePropertyValue $measurementPrefixMs -Force
        $runOut | Add-Member -NotePropertyName pin_mask -NotePropertyValue '0x0000FFFF' -Force
        $runOut | Add-Member -NotePropertyName process_affinity_mask -NotePropertyValue ('0x{0:X8}' -f [UInt64]$affinityReadback) -Force
        $runOut | Add-Member -NotePropertyName affinity_verified -NotePropertyValue ($affinityReadback -eq $pinMask) -Force
        $frequencyEndMhz = [AC1BenchTopology]::CurrentMhz($pinMask)
        if ($hostWindowClock) {
            $otherCpuSeconds = 0.0
            foreach ($p in Get-Process -ErrorAction SilentlyContinue) {
                if ($p.Id -ne $proc.Id -and $otherCpuStart.ContainsKey($p.Id)) { try { $otherCpuSeconds += [Math]::Max(0.0, $p.TotalProcessorTime.TotalSeconds - [double]$otherCpuStart[$p.Id]) } catch { } }
            }
            if ($hostWindowClock.Elapsed.TotalSeconds -gt 0) { $otherCpuPercent = 100.0 * $otherCpuSeconds / ($hostWindowClock.Elapsed.TotalSeconds * [Environment]::ProcessorCount) }
        }
        $runOut | Add-Member -NotePropertyName l3_domain_mask -NotePropertyValue '0x0000FFFF' -Force
        $runOut | Add-Member -NotePropertyName cpu_frequency_start_mhz -NotePropertyValue $frequencyStartMhz -Force
        $runOut | Add-Member -NotePropertyName cpu_frequency_end_mhz -NotePropertyValue $frequencyEndMhz -Force
        $runOut | Add-Member -NotePropertyName other_process_cpu_seconds -NotePropertyValue $otherCpuSeconds -Force
        $runOut | Add-Member -NotePropertyName other_process_cpu_percent -NotePropertyValue $otherCpuPercent -Force
        $runOut | Add-Member -NotePropertyName elapsed_seconds -NotePropertyValue $timer.Elapsed.TotalSeconds -Force
        $runOut | Add-Member -NotePropertyName exit_code -NotePropertyValue $proc.ExitCode -Force
        $runOut | Add-Member -NotePropertyName reached_end_present -NotePropertyValue $durationReached -Force
        $runOut | Add-Member -NotePropertyName safety_timeout_hit -NotePropertyValue $safetyTimeoutReached -Force
        $runOut | Add-Member -NotePropertyName evidence -NotePropertyValue $runDir -Force
        $runsOut += $runOut
    }
    $actualBuildDir = if (Test-Path "build/bench-$key-make/CMakeCache.txt") { "build/bench-$key-make" } else { $buildDir }
    $actualGenerator = 'unknown'
    if (Test-Path "$actualBuildDir/CMakeCache.txt") { $generatorLine = Select-String -Path "$actualBuildDir/CMakeCache.txt" -Pattern '^CMAKE_GENERATOR:INTERNAL=' | Select-Object -First 1; if ($generatorLine) { $actualGenerator = ($generatorLine.Line -split '=',2)[1] } }
    $obj = [ordered]@{ identity=[ordered]@{ sha=$sha; branch=$branch; dirty=$dirty; dirty_diff_sha256=$dirtyHash; config_id=$cfg; config=$cfgSpec; binary_sha256=$binaryHash; label=$Label; runtime_env=$Env }; machine=$machine; environment=[ordered]@{ workload=$Workload; safety_timeout_seconds=$Seconds; start_present=$StartPresent; end_present=$EndPresent; pin_mask='0x0000FFFF'; l3_domain_masks=$l3DomainMasks; frame_stats_flush_frames=$FrameStatsFlushFrames; startup_timeout_seconds=$StartupTimeoutSeconds; requested_runs=$Runs; replay=if ($Workload -eq 'gameplay') {'tools/input/to_gameplay.txt'} else {'none (menu remains idle)'}; profile='runtime-frame-stats'; profiler='none'; vsync='off (DXVK d3d9.presentInterval=0)'; host_load_override=[bool]$AllowBusy; actual_build_generator=$actualGenerator; gpu_sampling='not sampled' }; runs=$runsOut }
    $obj | ConvertTo-Json -Depth 20 | Set-Content (Join-Path $resultDir 'result.json') -Encoding UTF8
    & $Python -B tools/bench_report.py --summary $resultDir
    Write-Host "Benchmark evidence: $resultDir"
} finally { if ($ownedConfigProc -and -not $ownedConfigProc.HasExited) { Stop-OwnedTree $ownedConfigProc }; if ($benchLockStream) { $benchLockStream.Dispose(); Remove-Item -LiteralPath $benchLockPath -Force -ErrorAction SilentlyContinue }; Pop-Location }

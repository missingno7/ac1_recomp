param(
    [ValidateSet('Host', 'Android')][string]$Target = 'Host',
    [ValidateSet('Debug', 'RelWithDebInfo')][string]$BuildType = 'Debug',
    [ValidateRange(1, 64)][int]$Jobs = 8,
    [string]$Python = 'D:\Prog\dukemp_recomp\.venv\Scripts\python.exe',
    [string]$Sdk = "$env:LOCALAPPDATA\Android\Sdk",
    [switch]$Generate,
    [switch]$PrepareOnly,
    [switch]$CheckRepeat,
    [switch]$Run,
    # Whole-program flag contracts; 'cfg' restores per-function masks (all flags at calls/returns).
    [ValidateSet('off', 'straight', 'cfg', 'abi')][string]$FlagLiveness = 'abi',
    # Register locals (GPR+XMM) for every admitted function; 'off' restores CPU-struct registers.
    [string]$LocalRegs = 'all',
    [switch]$X87Inline,
    # x87 stack in block-local doubles without tag/env bookkeeping; 'exact' restores the full model.
    [ValidateSet('exact', 'native')][string]$X87 = 'native',
    # Dedicated count build only; default generated code has zero count hooks.
    [string]$ProfileCountEntries = '',
    # Checked per-access guest memory (diagnosable bad pointers) instead of the direct arena.
    [switch]$CheckedMemory,
    [string]$GameExe = 'C:\Program Files (x86)\GOG.com\Assassins Creed\AssassinsCreed_Dx9.exe',
    # Reuse an existing build's generator; Ninja is the default for a new one.
    [ValidateSet('Ninja', 'MinGW Makefiles')][string]$Generator
)
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$Timer = [System.Diagnostics.Stopwatch]::StartNew()
Push-Location $ProjectRoot
try {
    $NeedGenerate = $Generate -or -not (Test-Path 'work/lift_initial/initial.c')
    if (-not $NeedGenerate) {
        $ReportPath = 'work/lift_initial/lift-report.json'
        if (-not (Test-Path $ReportPath)) {
            $NeedGenerate = $true
        } else {
            $Report = Get-Content -LiteralPath $ReportPath -Raw | ConvertFrom-Json
            $ConfigHash = (Get-FileHash -LiteralPath 'config/ac1_dx9.json' -Algorithm SHA256).Hash.ToLowerInvariant()
            $DriverHash = (Get-FileHash -LiteralPath 'tools/lift_ac1.py' -Algorithm SHA256).Hash.ToLowerInvariant()
            $EmissionCacheHash = (Get-FileHash -LiteralPath 'tools/emission_cache.py' -Algorithm SHA256).Hash.ToLowerInvariant()
            $TablesHash = (Get-FileHash -LiteralPath 'config/startup_initializers.json' -Algorithm SHA256).Hash.ToLowerInvariant()
            $NeedGenerate = $Report.config_sha256 -ne $ConfigHash -or
                $Report.lift_driver_sha256 -ne $DriverHash -or $Report.initializer_tables_sha256 -ne $TablesHash -or
                $Report.generation_input_sha256.emission_cache -ne $EmissionCacheHash -or
                $Report.flag_liveness -ne $FlagLiveness -or
                $Report.local_regs.requested -ne $LocalRegs -or
                $(if ($Report.x87) { $Report.x87.mode } else { 'exact' }) -ne $X87
            $CountEntries = @($ProfileCountEntries.Split(',') | Where-Object { $_ } | ForEach-Object { ([Convert]::ToUInt32($_,16)).ToString('X8') } | Sort-Object -Unique)
            if ((@($Report.profile_count_entries) -join ',') -ne ($CountEntries -join ',')) { $NeedGenerate = $true }
        }
    }
    if ($NeedGenerate) {
        & $Python -B tools/analyze_pe.py
        if ($LASTEXITCODE -ne 0) { throw 'PE analysis failed' }
        & $Python -B tools/lift_ac1.py --flag-liveness $FlagLiveness --local-regs $LocalRegs --x87 $X87 "--profile-count-entries=$ProfileCountEntries"
        if ($LASTEXITCODE -ne 0) { throw 'AC1 generation failed' }
    }
    & $Python -B tools/prepare_runtime.py
    if ($LASTEXITCODE -ne 0) { throw 'CPU adapter preparation failed' }
    $BinkArgs = @('-B', 'tools/lift_bink.py')
    if (-not $Generate) { $BinkArgs += '--if-needed' }
    & $Python @BinkArgs
    if ($LASTEXITCODE -ne 0) { throw 'Bink module generation failed' }
    if ($PrepareOnly) {
        Write-Host ('Native inputs prepared in {0:N2} s' -f $Timer.Elapsed.TotalSeconds)
        return
    }
    $InputSeconds = $Timer.Elapsed.TotalSeconds
    $NativeBuildTimer = [System.Diagnostics.Stopwatch]::StartNew()
    $CMake = Join-Path $Sdk 'cmake/3.22.1/bin/cmake.exe'
    $Ninja = Join-Path $Sdk 'cmake/3.22.1/bin/ninja.exe'
    $BuildDir = "build/$($Target.ToLower())"
    if ($BuildType -ne 'Debug') { $BuildDir += "-$($BuildType.ToLower())" }
    if (-not $Generator) {
        $Generator = 'Ninja'
        if (Test-Path "$BuildDir/CMakeCache.txt") {
            $CachedGenerator = Select-String -LiteralPath "$BuildDir/CMakeCache.txt" -Pattern '^CMAKE_GENERATOR:INTERNAL=(.+)$'
            if ($CachedGenerator) { $Generator = $CachedGenerator.Matches[0].Groups[1].Value }
        }
    }
    $MakeProgram = if ($Generator -eq 'MinGW Makefiles') { 'C:/msys64/mingw64/bin/mingw32-make.exe' } else { $Ninja }
    $ConfigureArgs = @('-S', '.', '-B', $BuildDir, '-G', $Generator, "-DCMAKE_MAKE_PROGRAM=$MakeProgram", "-DCMAKE_BUILD_TYPE=$BuildType")
    # Explicit, because an option default never overrides an existing CMake cache entry.
    $ConfigureArgs += "-DAC1_FORCE_INLINE_X87=$(if ($X87Inline) { 'ON' } else { 'OFF' })"
    $ConfigureArgs += "-DAC1_DIRECT_MEMORY=$(if ($CheckedMemory) { 'OFF' } else { 'ON' })"
    if ($Target -eq 'Host') {
        $ConfigureArgs += '-DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe'
        $ConfigureArgs += '-DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe'
    } else {
        $ConfigureArgs += @("-DCMAKE_TOOLCHAIN_FILE=$Sdk/ndk/28.2.13676358/build/cmake/android.toolchain.cmake", '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-28')
    }
    & $CMake @ConfigureArgs
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    & $CMake --build $BuildDir --parallel $Jobs
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed' }
    Write-Host ('Native build completed in {0:N2} s (input preparation {1:N2} s; configure/compile/link {2:N2} s)' -f
        $Timer.Elapsed.TotalSeconds, $InputSeconds, $NativeBuildTimer.Elapsed.TotalSeconds)
    if ($CheckRepeat) {
        if ($Target -ne 'Host') { throw '-CheckRepeat applies to Host' }
        & $CMake --build $BuildDir --target ac1_repeat_run --parallel $Jobs
        if ($LASTEXITCODE -ne 0) { throw 'Repeat-run check build failed' }
        & (Join-Path $ProjectRoot "$BuildDir/ac1_repeat_run.exe") $GameExe
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    if ($Run) {
        if ($Target -ne 'Host') { throw '-Run applies to Host; use the Android packaging script for APKs' }
        $RunErrorPreference = $ErrorActionPreference
        try {
            # Windows PowerShell represents redirected native stderr as error
            # records. The guest diagnostics use stderr even on normal startup.
            $ErrorActionPreference = 'Continue'
            & (Join-Path $ProjectRoot "$BuildDir/ac1_startup.exe") $GameExe 2>&1 |
                ForEach-Object { "$_" } |
                Tee-Object -FilePath (Join-Path $ProjectRoot 'work/startup.log')
            $RunExitCode = $LASTEXITCODE
        } finally { $ErrorActionPreference = $RunErrorPreference }
        if ($RunExitCode -ne 0) { exit $RunExitCode }
    }
} finally { Pop-Location }

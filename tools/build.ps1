param(
    [ValidateSet('Host', 'Android')][string]$Target = 'Host',
    [string]$Python = 'D:\Prog\dukemp_recomp\.venv\Scripts\python.exe',
    [string]$Sdk = "$env:LOCALAPPDATA\Android\Sdk",
    [switch]$Generate,
    [switch]$PrepareOnly,
    [switch]$CheckRepeat,
    [switch]$Run,
    [string]$GameExe = 'C:\Program Files (x86)\GOG.com\Assassins Creed\AssassinsCreed_Dx9.exe'
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
            $TablesHash = (Get-FileHash -LiteralPath 'config/startup_initializers.json' -Algorithm SHA256).Hash.ToLowerInvariant()
            $NeedGenerate = $Report.config_sha256 -ne $ConfigHash -or
                $Report.lift_driver_sha256 -ne $DriverHash -or $Report.initializer_tables_sha256 -ne $TablesHash
        }
    }
    if ($NeedGenerate) {
        & $Python -B tools/analyze_pe.py
        if ($LASTEXITCODE -ne 0) { throw 'PE analysis failed' }
        & $Python -B tools/lift_ac1.py
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
    $CMake = Join-Path $Sdk 'cmake/3.22.1/bin/cmake.exe'
    $Ninja = Join-Path $Sdk 'cmake/3.22.1/bin/ninja.exe'
    $BuildDir = "build/$($Target.ToLower())"
    $ConfigureArgs = @('-S', '.', '-B', $BuildDir, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$Ninja", '-DCMAKE_BUILD_TYPE=Debug')
    if ($Target -eq 'Host') {
        $ConfigureArgs += '-DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe'
        $ConfigureArgs += '-DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe'
    } else {
        $ConfigureArgs += @("-DCMAKE_TOOLCHAIN_FILE=$Sdk/ndk/28.2.13676358/build/cmake/android.toolchain.cmake", '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-28')
    }
    & $CMake @ConfigureArgs
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    & $CMake --build $BuildDir --parallel 8
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed' }
    Write-Host ('Native build completed in {0:N2} s' -f $Timer.Elapsed.TotalSeconds)
    if ($CheckRepeat) {
        if ($Target -ne 'Host') { throw '-CheckRepeat applies to Host' }
        & $CMake --build $BuildDir --target ac1_repeat_run --parallel 8
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

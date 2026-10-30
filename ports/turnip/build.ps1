param(
    [string]$AndroidSdk = "$env:LOCALAPPDATA/Android/Sdk",
    [string]$NdkVersion = '28.2.13676358',
    [ValidateRange(29,36)][int]$Api = 29,
    [string]$Python = 'python.exe',
    [string]$HostTools = '',
    [int]$Jobs = 8,
    [ValidateSet('Ninja','Make')][string]$SupportBuilder = 'Ninja',
    [switch]$ConfigureOnly,
    [switch]$DirectCommands,
    [switch]$SupportOnly
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path "$PSScriptRoot/../..").Path
$Build = "$Root/work/build/turnip-arm64"
$Ndk = "$AndroidSdk/ndk/$NdkVersion"
$Bin = "$Ndk/toolchains/llvm/prebuilt/windows-x86_64/bin".Replace('\','/')
$Cmake = "$AndroidSdk/cmake/3.22.1/bin/cmake.exe"
$Ninja = "$AndroidSdk/cmake/3.22.1/bin/ninja.exe"
$env:PYTHONDONTWRITEBYTECODE = '1'
if ($HostTools) { $env:PATH = "$HostTools;$env:PATH" }
function Run([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed ($LASTEXITCODE): $Arguments" }
}
$Pins = Get-Content "$PSScriptRoot/sources.json" -Raw | ConvertFrom-Json
foreach ($Name in @('mesa','adrenotools','linkernsbypass')) {
    $Pin = $Pins.$Name
    $Dir = "$Root/work/deps/$($Pin.directory)"
    if (!(Test-Path "$Dir/.git")) {
        $CloneArgs = @('-c','http.proxy=','clone')
        if ($Pin.tag) { $CloneArgs += @('--depth','1','--branch',$Pin.tag) }
        Run git ($CloneArgs + @($Pin.url,$Dir))
        Run git @('-C',$Dir,'checkout','--detach',$Pin.commit)
    }
    $Revision = (& git -C $Dir rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $Revision -ne $Pin.commit) { throw "Unexpected $Name HEAD: $Revision" }
    if (& git -C $Dir status --porcelain --untracked-files=no) { throw "Modified pinned $Name source: $Dir" }
}
New-Item -ItemType Directory -Force "$Build/package/arm64-v8a" | Out-Null
New-Item -ItemType Directory -Force "$Build/pkgconfig" | Out-Null
# Android provides libz and its headers in the NDK. Do not pick host zlib.pc.
$ZlibHeader = Get-Content "$Ndk/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/include/zlib.h" -Raw
$ZlibVersion = [regex]::Match($ZlibHeader, '#define ZLIB_VERSION "([^"]+)"').Groups[1].Value
if (!$ZlibVersion) { throw 'NDK zlib version not found' }
[IO.File]::WriteAllText("$Build/pkgconfig/zlib.pc", "Name: zlib`nDescription: Android NDK system zlib`nVersion: $ZlibVersion`nLibs: -lz`nCflags:`n", [Text.UTF8Encoding]::new($false))
$Mesa = "$Root/work/deps/$($Pins.mesa.directory)"
Run $Python @("$PSScriptRoot/prepare_mesa.py", $Mesa, "$Root/work/turnip/mesa-source")
$Mesa = "$Root/work/turnip/mesa-source"
$env:MESA_GIT_SHA1_OVERRIDE = $Pins.mesa.commit
$Support = "$Build/support-$($SupportBuilder.ToLowerInvariant())"
$Generator = if ($SupportBuilder -eq 'Make') { 'MinGW Makefiles' } else { 'Ninja' }
$Make = if ($SupportBuilder -eq 'Make') { (Get-Command mingw32-make.exe -ErrorAction Stop).Source } else { $Ninja }
Run $Cmake @('-S',$PSScriptRoot,'-B',$Support,'-G',$Generator,"-DCMAKE_MAKE_PROGRAM=$Make",
    "-DCMAKE_TOOLCHAIN_FILE=$Ndk/build/cmake/android.toolchain.cmake",'-DANDROID_ABI=arm64-v8a',
    "-DANDROID_PLATFORM=$Api",'-DANDROID_STL=c++_static','-DCMAKE_BUILD_TYPE=Release',"-DMESA_SOURCE=$Mesa")
Run $Cmake @('--build',$Support,'--parallel',"$Jobs")
Get-ChildItem $Support -Recurse -Filter '*.so' | ForEach-Object {
    Copy-Item $_.FullName "$Build/package/arm64-v8a/$($_.Name)"
}
Copy-Item "$Support/ac1_vulkan_probe" "$Build/ac1_vulkan_probe"
if ($SupportOnly) { return }
$Cross = @"
[binaries]
c = ['$Bin/clang.exe', '--target=aarch64-linux-android$Api']
cpp = ['$Bin/clang++.exe', '--target=aarch64-linux-android$Api']
ar = '$Bin/llvm-ar.exe'
strip = '$Bin/llvm-strip.exe'
nm = '$Bin/llvm-nm.exe'
pkg-config = 'pkgconf'
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$($Build.Replace('\','/'))/pkgconfig'
[built-in options]
c_args = ['-fPIC', '-ffile-prefix-map=$($Root.Replace('\','/'))=.', '-Wno-error']
cpp_args = ['-fPIC', '-ffile-prefix-map=$($Root.Replace('\','/'))=.']
c_link_args = ['-Wl,-z,max-page-size=16384']
cpp_link_args = ['-static-libstdc++', '-Wl,-z,max-page-size=16384']
"@
[IO.File]::WriteAllText("$Build/android.ini", $Cross.Replace("`r`n","`n"), [Text.UTF8Encoding]::new($false))
$env:NINJA = $Ninja
$MesaBuild = "$Build/mesa-release"
$Setup = @('-m','mesonbuild.mesonmain','setup',$MesaBuild,$Mesa,'--cross-file',"$Build/android.ini",
    "-Dpkg_config_path=$($Build.Replace('\','/'))/pkgconfig",
    '--buildtype=release','--wrap-mode=nodownload','-Dstrip=true','-Db_lto=false',
    '-Dvulkan-drivers=freedreno','-Dfreedreno-kmds=kgsl','-Dgallium-drivers=',
    '-Dplatforms=android','-Dandroid-stub=true','-Dandroid-strict=false',"-Dplatform-sdk-version=$Api",
    '-Dopengl=false','-Dgles1=disabled','-Dgles2=disabled','-Degl=disabled','-Dgbm=disabled','-Dglx=disabled',
    '-Dllvm=disabled','-Dshared-glapi=disabled','-Dvideo-codecs=','-Dzstd=disabled','-Dzlib=enabled',
    '-Dshader-cache=enabled','-Dbuild-tests=false','-Dlibunwind=disabled','-Dandroid-libbacktrace=disabled',
    '-Dandroid-libperfetto=disabled','-Dperfetto=false','-Dvalgrind=disabled')
if (Test-Path "$MesaBuild/meson-private/coredata.dat") { $Setup += @('--reconfigure','--clearcache') }
Run $Python $Setup
if ($ConfigureOnly) { return }
if ($DirectCommands) {
    Run $Python @("$PSScriptRoot/ninja_commands.py", $MesaBuild, $Ninja, "$Jobs")
} else {
    Run $Ninja @('-C',$MesaBuild,'-j',"$Jobs",'src/freedreno/vulkan/libvulkan_freedreno.so','src/freedreno/vulkan/freedreno_icd.armv8-a.json')
}
Run $Python @("$PSScriptRoot/stage.py", $MesaBuild, "$Build/package")
Run "$Bin/llvm-strip.exe" @('--strip-unneeded',"$Build/package/arm64-v8a/libvulkan_freedreno.so")
Get-ChildItem "$Build/package/arm64-v8a" | ForEach-Object { Run "$Bin/llvm-strip.exe" @('--strip-unneeded',$_.FullName) }
Get-ChildItem "$Build/package/arm64-v8a" | Select-Object Name,Length

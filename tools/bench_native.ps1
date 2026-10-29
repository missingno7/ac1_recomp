param(
    [string]$Binary = 'build/host-relwithdebinfo/ac1_startup.exe',
    [string[]]$Overrides = @(),
    [ValidateRange(3,50)][int]$Runs = 3,
    [ValidateRange(60,86400)][int]$Seconds = 60,
    [string]$Label = 'native',
    [switch]$AllowConcurrent,
    [switch]$AllowDirty
)
$ErrorActionPreference = 'Stop'
# Each pair gets a unique immutable result directory. The same stripped binary
# and runtime environment are used throughout; only the selected VAs differ.
$variants = @('1') + $Overrides
for ($round = 1; $round -le $Runs; ++$round) {
    foreach ($variant in $variants) {
        $tag = if ($variant -eq '1') { 'all' } else { $variant -replace '[^0-9a-fA-F]', '' }
        foreach ($setting in @('0', $variant)) {
            $side = if ($setting -eq '0') { 'off' } else { 'on' }
            & "$PSScriptRoot/bench_gameplay.ps1" -Binary $Binary -Runs 1 -Seconds $Seconds `
                -AllowConcurrent:$AllowConcurrent -AllowDirty:$AllowDirty -Label "$Label-$tag-$round-$side" `
                -Env @{ AC1_NATIVE_OVERRIDES=$setting; AC1_NATIVE_VERIFY='0'; AC1_NATIVE_STATS='0' }
            if ($LASTEXITCODE) { throw "Native benchmark failed ($tag round $round $side)" }
        }
    }
}

param(
    [string]$Game,
    [string]$Module,
    [string]$UserDirectory,
    [string]$LoadState,
    [string]$PerformanceDirectory
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Game) { $Game = Join-Path $repoRoot 'extracted/Colo-Fresh' }
if (-not $Module) { $Module = Join-Path $repoRoot 'build/freecam-module/gGC6E01_recomp.dll' }
if (-not $UserDirectory) { $UserDirectory = Join-Path $repoRoot 'build/freecam-user' }
$runner = Join-Path $repoRoot 'lib/ModernGekko/build/moderngekko-run-display.exe'
$mod = Join-Path $repoRoot 'build/freecam-arrival-mods/packages/pokemon-colosseum-freecam.mgm'
foreach ($required in @($Game, $Module, $runner, (Join-Path $mod 'mod.dll'))) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing free-camera prerequisite: $required. See mods/FREECAM.md." }
}
New-Item -ItemType Directory -Force -Path $UserDirectory | Out-Null
$controllerConfig = Join-Path $UserDirectory 'Config'
$sourceConfig = Join-Path $repoRoot 'User/Config'
if (-not (Test-Path -LiteralPath $controllerConfig) -and (Test-Path -LiteralPath $sourceConfig)) {
    Copy-Item -LiteralPath $sourceConfig -Destination $controllerConfig -Recurse
}
$runnerArgs = @('--game', $Game, '--module', $Module, '--user-dir', $UserDirectory,
                '--no-mods', '--mods', $mod)
if ($LoadState) {
    $runnerArgs += @('--load-state', (Resolve-Path -LiteralPath $LoadState).Path)
}
if ($PerformanceDirectory) {
    New-Item -ItemType Directory -Force -Path $PerformanceDirectory | Out-Null
    $runnerArgs += @('--automation-dir', (Resolve-Path -LiteralPath $PerformanceDirectory).Path)
}
& $runner @runnerArgs
if ($LASTEXITCODE -ne 0) { throw "Free-camera runner exited with code $LASTEXITCODE" }

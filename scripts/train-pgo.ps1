<#
.SYNOPSIS
Optional profile-guided optimization of the game module, trained on your own play.

.DESCRIPTION
PGO measured about +5% average frame rate across a spread of scenes here. It is
opt-in and it is local, because a profile cannot be shipped: it is
instrumentation counters over the recompiled game code, so it is derived from
the game itself and is not ours to distribute. Every player who wants it trains
their own.

There are two phases with you playing in between:

  1. train   Builds an instrumented module and installs it. Play the game
             normally - it writes profile data as it runs. The instrumented
             module is SLOW; that is expected and is not what you keep.

  2. finish  Merges what you recorded, rebuilds an optimized module, and
             installs it. Deletes nothing until the new module is built.

Train on what you actually play. A profile recorded entirely in one town
optimizes for that town; the measurement above came from a deliberate spread of
field, menu, and battle scenes. Ten to fifteen minutes across a few different
areas is a reasonable target, including at least one battle.

.EXAMPLE
  scripts\train-pgo.ps1 -Phase train
  # ... play for a while, then quit the game ...
  scripts\train-pgo.ps1 -Phase finish
#>
param(
  [Parameter(Mandatory = $true)][ValidateSet('train', 'finish')][string]$Phase,
  [string]$GameRoot,
  [string]$ProfileDir,
  [int]$ParallelJobs = 0
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$moduleOutput = Join-Path $projectRoot 'build\modules'
$module60Root = Join-Path $projectRoot 'b60'
$pgoRoot = if ($ProfileDir) { $ProfileDir } else { Join-Path $projectRoot 'build\pgo' }
$profileRaw = Join-Path $pgoRoot 'raw'
$profileData = Join-Path $pgoRoot 'merged.profdata'
$port = Join-Path $projectRoot 'lib\ModernGekko\build\moderngekko-port.exe'
$substManifest = Join-Path $projectRoot 'patches\GC6E01-native-substitutions.csv'
$dolPatchManifest = Join-Path $projectRoot 'patches\GC6E01-native-widescreen.csv'

if (-not (Test-Path -LiteralPath $port)) {
  throw "moderngekko-port is not built yet. Run scripts\setup-windows.ps1 first."
}
if (-not $GameRoot) { $GameRoot = Join-Path $projectRoot 'extracted\Pokemon-Colosseum-USA' }
if (-not (Test-Path -LiteralPath $GameRoot)) {
  throw "Game root not found: $GameRoot. Pass -GameRoot <path>."
}
if ($ParallelJobs -gt 0) {
  $env:MODERNGEKKO_BUILD_JOBS = $ParallelJobs.ToString([Globalization.CultureInfo]::InvariantCulture)
}

# The module has to be built exactly as the shipped one is, or the profile
# describes different code: same DOL patches, same native substitutions.
function Get-BaseArguments([string]$output) {
  $arguments = @('build', $GameRoot, '--backend', 'c', '--toolchain', 'clang',
                 '--opt-level', '2', '--output', $output)
  foreach ($patch in (Import-Csv -LiteralPath $dolPatchManifest)) {
    $arguments += @('--dol-patch', "$($patch.address)=$($patch.replacement)")
  }
  foreach ($substitution in (Import-Csv -LiteralPath $substManifest)) {
    $arguments += @('--native-subst', "$($substitution.guest):$($substitution.symbol)")
  }
  return $arguments
}

# The 60 FPS variant takes no extra DOL patch. Baking 80005D98=38600000 into
# the module contradicts the guest RAM the runner actually presents, which drops
# chunk [0x800055E0,0x800095E0) to the interpreter for the session; 60 FPS comes
# from the runner's [QualityOfLife] sixty_fps switch instead.
if ($Phase -eq 'train') {
  New-Item -ItemType Directory -Path $profileRaw -Force | Out-Null
  Get-ChildItem -LiteralPath $profileRaw -Filter '*.profraw' -ErrorAction SilentlyContinue |
    Remove-Item -Force
  foreach ($target in @(@{ out = $moduleOutput; extra = @() },
                        @{ out = $module60Root; extra = @() })) {
    $arguments = (Get-BaseArguments $target.out) + $target.extra +
                 @('--pgo-generate', $profileRaw)
    & $port @arguments
    if ($LASTEXITCODE -ne 0) { throw 'Instrumented module build failed.' }
  }
  # The instrumented module only records if this is set when the game runs, and
  # %p keeps one process from overwriting another's file.
  $env:LLVM_PROFILE_FILE = Join-Path $profileRaw 'colo-%p.profraw'
  [Environment]::SetEnvironmentVariable('LLVM_PROFILE_FILE',
    (Join-Path $profileRaw 'colo-%p.profraw'), 'User')
  Write-Host ''
  Write-Host 'Instrumented module installed. Now play the game normally.'
  Write-Host 'It will run noticeably slower while training - that is expected.'
  Write-Host 'Cover a few different areas, including at least one battle.'
  Write-Host ''
  Write-Host "Profiles are being written to: $profileRaw"
  Write-Host 'When you have played enough, quit the game and run:'
  Write-Host '    scripts\train-pgo.ps1 -Phase finish'
  return
}

# finish
$raw = @(Get-ChildItem -LiteralPath $profileRaw -Filter '*.profraw' -ErrorAction SilentlyContinue)
if ($raw.Count -eq 0) {
  throw ("No profile data in $profileRaw. Run -Phase train, play the game, then " +
         'try again. If you did play, LLVM_PROFILE_FILE was probably not set in ' +
         'the session that launched the game - reopen your terminal and replay.')
}
$megabytes = [math]::Round((($raw | Measure-Object Length -Sum).Sum / 1MB), 1)
Write-Host "Merging $($raw.Count) profile file(s), $megabytes MB."

$profdata = Get-Command 'llvm-profdata' -ErrorAction SilentlyContinue
if (-not $profdata) {
  $bundled = Join-Path $projectRoot 'extern\clang+llvm-20.1.8-x86_64-pc-windows-msvc\bin\llvm-profdata.exe'
  if (Test-Path -LiteralPath $bundled) { $profdata = $bundled }
  else { throw 'llvm-profdata not found. It ships with LLVM; add it to PATH.' }
} else { $profdata = $profdata.Source }

& $profdata merge "-output=$profileData" ($raw | ForEach-Object { $_.FullName })
if ($LASTEXITCODE -ne 0) { throw 'llvm-profdata merge failed.' }

foreach ($target in @(@{ out = $moduleOutput; extra = @() },
                      @{ out = $module60Root; extra = @() })) {
  $arguments = (Get-BaseArguments $target.out) + $target.extra + @('--pgo-use', $profileData)
  & $port @arguments
  if ($LASTEXITCODE -ne 0) { throw 'Optimized module build failed.' }
}

# Only now is it safe to stop recording: the optimized modules exist.
Remove-Item Env:\LLVM_PROFILE_FILE -ErrorAction SilentlyContinue
[Environment]::SetEnvironmentVariable('LLVM_PROFILE_FILE', $null, 'User')
Write-Host ''
Write-Host 'Optimized module installed. Launch the game normally.'
Write-Host "Profile kept at $profileData - rerun -Phase finish to rebuild from it,"
Write-Host 'or -Phase train again to record fresh play.'

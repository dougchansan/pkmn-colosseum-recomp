[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Iso,

    [ValidateRange(1, 16)]
    [int]$ParallelJobs = 4
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$isoPath = [IO.Path]::GetFullPath($Iso)
$dolRecompRoot = Join-Path $projectRoot 'lib\DolRecomp'
$modernGekkoRoot = Join-Path $projectRoot 'lib\ModernGekko'
$dolRecompBuild = Join-Path $dolRecompRoot 'build'
$dolRecomp = Join-Path $dolRecompBuild 'dolrecomp.exe'
$gameRoot = Join-Path $projectRoot 'extracted\Pokemon-Colosseum-USA'
$hostPatchManifest = Get-Content -LiteralPath `
    (Join-Path $projectRoot 'mods\host-patches\manifest.json') -Raw | ConvertFrom-Json

if (-not (Test-Path -LiteralPath $isoPath -PathType Leaf)) {
    throw "Pokemon Colosseum ISO not found: $isoPath"
}
foreach ($command in @('git', 'cmake', 'ninja', 'python')) {
    if (-not (Get-Command $command -ErrorAction SilentlyContinue)) {
        throw "Required build tool is not on PATH: $command"
    }
}

if (-not $env:INCLUDE) {
    $environmentDump = & cmd.exe /d /c "`"$PSScriptRoot\vsdev-dump.bat`""
    foreach ($line in $environmentDump) {
        if ($line -match '^([^=]+)=(.*)$') {
            $key = $Matches[1]
            if ($key -in @(
                    'PATH', 'Path', 'INCLUDE', 'LIB', 'LIBPATH', 'VCINSTALLDIR',
                    'VCToolsInstallDir', 'WindowsSdkDir', 'WindowsSDKVersion',
                    'UCRTVersion', 'VSINSTALLDIR'
                )) {
                [Environment]::SetEnvironmentVariable($key, $Matches[2], 'Process')
            }
        }
    }
    if (-not $env:INCLUDE) {
        throw 'Visual Studio C++ Build Tools and the Windows SDK were not found.'
    }
}
if (-not (Get-Command clang -ErrorAction SilentlyContinue)) {
    throw 'Clang is required to compile the portable C game module.'
}

Push-Location $projectRoot
try {
    if (-not (Test-Path -LiteralPath (Join-Path $dolRecompRoot 'CMakeLists.txt') -PathType Leaf)) {
        New-Item -ItemType Directory -Path (Join-Path $projectRoot 'lib') -Force | Out-Null
        git clone https://github.com/ExpansionPak/DolRecomp.git $dolRecompRoot
        if ($LASTEXITCODE -ne 0) { throw 'DolRecomp clone failed.' }
        git -C $dolRecompRoot checkout --detach $hostPatchManifest.dolRecomp.baseCommit
        if ($LASTEXITCODE -ne 0) { throw 'DolRecomp release revision checkout failed.' }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $modernGekkoRoot 'CMakeLists.txt') -PathType Leaf)) {
        New-Item -ItemType Directory -Path (Join-Path $projectRoot 'lib') -Force | Out-Null
        git clone https://github.com/ExpansionPak/ModernGekko.git $modernGekkoRoot
        if ($LASTEXITCODE -ne 0) { throw 'ModernGekko clone failed.' }
        git -C $modernGekkoRoot checkout --detach $hostPatchManifest.modernGekko.baseCommit
        if ($LASTEXITCODE -ne 0) { throw 'ModernGekko release revision checkout failed.' }
        git -C $modernGekkoRoot submodule update --init --recursive
        if ($LASTEXITCODE -ne 0) { throw 'ModernGekko public dependencies failed to initialize.' }
    }

    & (Join-Path $projectRoot 'mods\host-patches\apply-host-patches.ps1') `
        -ModernGekkoSource $modernGekkoRoot
    if ($LASTEXITCODE -ne 0) { throw 'Colosseum host patch setup failed.' }

    cmake -S $dolRecompRoot -B $dolRecompBuild -G Ninja `
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DDOLRECOMP_ENABLE_LLVM=OFF
    if ($LASTEXITCODE -ne 0) { throw 'DolRecomp configuration failed.' }
    cmake --build $dolRecompBuild --parallel $ParallelJobs --target dolrecomp
    if ($LASTEXITCODE -ne 0) { throw 'DolRecomp build failed.' }

    $mainDol = Join-Path $gameRoot 'sys\main.dol'
    if (-not (Test-Path -LiteralPath $mainDol -PathType Leaf)) {
        New-Item -ItemType Directory -Force -Path $gameRoot | Out-Null
        & $dolRecomp extract $isoPath $gameRoot
        if ($LASTEXITCODE -ne 0) { throw 'Game extraction failed.' }
    }

    $bootBin = Join-Path $gameRoot 'sys\boot.bin'
    if (-not (Test-Path -LiteralPath $bootBin -PathType Leaf)) {
        throw "Extracted game is incomplete: $bootBin is missing."
    }
    $bootBytes = [IO.File]::ReadAllBytes($bootBin)
    $discId = [Text.Encoding]::ASCII.GetString($bootBytes, 0, 6)
    if ($discId -ne 'GC6E01') {
        throw "This release requires the USA Pokemon Colosseum disc (GC6E01); found $discId."
    }

    & (Join-Path $PSScriptRoot 'package-imgui-launcher.ps1') `
        -ParallelJobs $ParallelJobs
    if ($LASTEXITCODE -ne 0) { throw 'Release packaging failed.' }
}
finally {
    Pop-Location
}

$launcher = Join-Path $projectRoot 'build\release\PokemonColosseumRecomp\PokemonColosseumRecomp.exe'
Write-Output "Setup complete. Launch: $launcher"

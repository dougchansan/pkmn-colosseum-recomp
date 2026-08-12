param(
    [switch]$InstallRepositoryRoot,
    [switch]$SkipModuleBuild,

    [ValidateRange(1, 16)]
    [int]$ParallelJobs = 4
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$modernGekkoRoot = Join-Path $projectRoot 'lib\ModernGekko'
$modernGekkoBuild = Join-Path $modernGekkoRoot 'build'
$modsRoot = Join-Path $projectRoot 'mods'
$modsBuild = Join-Path $projectRoot 'build\mods'
$packageRoot = Join-Path $projectRoot 'build\release\PokemonColosseumRecomp'
$gameRoot = Join-Path $projectRoot 'extracted\Pokemon-Colosseum-USA'
$marker = Join-Path $projectRoot 'build\modules\GC6E01\active-module-c.txt'
$module60Root = Join-Path $projectRoot 'b60'
$marker60 = Join-Path $module60Root 'GC6E01\active-module-c.txt'
$dolPatchManifest = Join-Path $projectRoot 'patches\GC6E01-native-widescreen.csv'

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
        throw 'Visual Studio developer environment did not provide Windows SDK include paths.'
    }
}

if (-not (Test-Path -LiteralPath $gameRoot -PathType Container)) {
    throw "Missing extracted game: $gameRoot"
}
if (-not (Test-Path -LiteralPath $dolPatchManifest -PathType Leaf)) {
    throw "Missing Colosseum DOL patch manifest: $dolPatchManifest"
}

cmake -S $modernGekkoRoot -B $modernGekkoBuild -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DBUILD_TESTING=OFF `
    '-DMODERNGEKKO_FRONTEND_NAME=Pokemon Colosseum Recomp' `
    -DMODERNGEKKO_LAUNCHER_OUTPUT_NAME=PokemonColosseumRecomp `
    -DMODERNGEKKO_RUNNER_OUTPUT_NAME=moderngekko-run `
    -DMODERNGEKKO_USER_DIRECTORY_NAME=PokemonColosseumRecomp `
    -DMODERNGEKKO_REQUIRED_DISC_ID=GC6E01 `
    '-DMODERNGEKKO_DEFAULT_WINDOW_TITLE=Pokemon Colosseum Recomp [30 FPS]' `
    '-DMODERNGEKKO_LOG_FILENAME=PokemonColosseumRecomp.log' `
    "-DMODERNGEKKO_DOL_PATCH_MANIFEST=$dolPatchManifest" `
    -DMODERNGEKKO_PORTABLE_DEFAULT_GAME=ON `
    -DMODERNGEKKO_PORTABLE_USER_DIRECTORY=ON `
    -DMODERNGEKKO_GAMECUBE_CONTROLLERS=ON `
    '-DMODERNGEKKO_NATIVE_ULTRAWIDE_MOD_PACKAGE=Mods/Optional/pokemon-colosseum-ultrawide.mgm' `
    '-DMODERNGEKKO_TEXT_UPSCALE_MOD_PACKAGE=Mods/Optional/pokemon-colosseum-hd-text.mgm' `
    '-DMODERNGEKKO_MINIMAP_MOD_PACKAGE=Mods/Optional/pokemon-colosseum-minimap.mgm'
if ($LASTEXITCODE -ne 0) {
    throw 'CMake configuration failed.'
}

cmake --build $modernGekkoBuild --parallel $ParallelJobs `
    --target moderngekko-launcher moderngekko-run moderngekko-port
if ($LASTEXITCODE -ne 0) {
    throw 'ImGui frontend build failed.'
}

cmake -S $modsRoot -B $modsBuild -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    "-DMODERNGEKKO_SOURCE_DIR=$modernGekkoRoot"
if ($LASTEXITCODE -ne 0) {
    throw 'ModernGekko mod package configuration failed.'
}
cmake --build $modsBuild --parallel $ParallelJobs
if ($LASTEXITCODE -ne 0) {
    throw 'ModernGekko mod package build failed.'
}

# Static recompilation must see the same executable hook that the packaged
# runner applies to the user's extracted DOL. Otherwise the runtime AR code
# branches into guest instructions that were data when DolRecomp analyzed the
# original image, producing a stretched fallback projection or an interpreter
# trap at 0x80005300.
if (-not $SkipModuleBuild) {
    $port = Join-Path $modernGekkoBuild 'moderngekko-port.exe'
    $moduleOutput = Join-Path $projectRoot 'build\modules'
    $dolPatches = Import-Csv -LiteralPath $dolPatchManifest
    $env:MODERNGEKKO_BUILD_JOBS = $ParallelJobs.ToString(
        [Globalization.CultureInfo]::InvariantCulture)
    $portArguments = @(
        'build', $gameRoot,
        '--backend', 'c',
        '--toolchain', 'clang',
        '--opt-level', '2',
        '--output', $moduleOutput
    )
    foreach ($patch in $dolPatches) {
        $portArguments += @('--dol-patch', "$($patch.address)=$($patch.replacement)")
    }
    & $port @portArguments
    if ($LASTEXITCODE -ne 0) {
        throw 'Stable C module build failed.'
    }

    $port60Arguments = @($portArguments)
    $port60Arguments[$port60Arguments.IndexOf($moduleOutput)] = $module60Root
    $port60Arguments += @('--dol-patch', '80005D98=38600000')
    & $port @port60Arguments
    if ($LASTEXITCODE -ne 0) {
        throw 'Optional 60 FPS C module build failed.'
    }
}

if (-not (Test-Path -LiteralPath $marker -PathType Leaf)) {
    throw "Missing stable C module marker after build: $marker"
}
$moduleValue = (Get-Content -LiteralPath $marker -Raw).Trim()
$modulePath = if ([IO.Path]::IsPathRooted($moduleValue)) {
    $moduleValue
} else {
    Join-Path $projectRoot $moduleValue
}
if (-not (Test-Path -LiteralPath $modulePath -PathType Leaf)) {
    throw "Stable C module does not exist: $modulePath"
}
$module60Value = (Get-Content -LiteralPath $marker60 -Raw).Trim()
$module60Path = if ([IO.Path]::IsPathRooted($module60Value)) {
    $module60Value
} else {
    Join-Path $projectRoot $module60Value
}
if (-not (Test-Path -LiteralPath $module60Path -PathType Leaf)) {
    throw "Optional 60 FPS C module does not exist: $module60Path"
}

$launcher = Join-Path $modernGekkoBuild 'PokemonColosseumRecomp.exe'
$runner = Join-Path $modernGekkoBuild 'moderngekko-run.exe'
$sys = Join-Path $modernGekkoBuild 'Sys'
$patches = Join-Path $modernGekkoBuild 'patches'
$modPackages = Join-Path $modsBuild 'packages'
foreach ($required in @($launcher, $runner, $sys, $patches, $modPackages)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Missing package input: $required"
    }
}

function Install-Package([string]$destination) {
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Copy-Item -LiteralPath $launcher -Destination (Join-Path $destination 'PokemonColosseumRecomp.exe') -Force
    Copy-Item -LiteralPath $runner -Destination (Join-Path $destination 'moderngekko-run.exe') -Force
    Copy-Item -LiteralPath $modulePath -Destination (Join-Path $destination 'gGC6E01_recomp.dll') -Force
    Copy-Item -LiteralPath $module60Path `
        -Destination (Join-Path $destination 'gGC6E01_recomp_60fps.dll') -Force
    Copy-Item -LiteralPath $sys -Destination $destination -Recurse -Force
    Copy-Item -LiteralPath $patches -Destination $destination -Recurse -Force
    foreach ($releaseDocument in @('LICENSE', 'THIRD_PARTY_NOTICES.md', 'VERSION')) {
        Copy-Item -LiteralPath (Join-Path $projectRoot $releaseDocument) `
            -Destination $destination -Force
    }
    $optionalMods = Join-Path $destination 'Mods\Optional'
    New-Item -ItemType Directory -Path $optionalMods -Force | Out-Null
    foreach ($sourcePackage in Get-ChildItem -LiteralPath $modPackages -Directory -Filter '*.mgm') {
        $sourceLibrary = Join-Path $sourcePackage.FullName 'mod.dll'
        if (-not (Test-Path -LiteralPath $sourceLibrary -PathType Leaf)) {
            throw "Missing Windows mod library: $sourceLibrary"
        }
        $destinationPackage = Join-Path $optionalMods $sourcePackage.Name
        New-Item -ItemType Directory -Path $destinationPackage -Force | Out-Null
        Copy-Item -LiteralPath $sourceLibrary -Destination $destinationPackage -Force
        $sourceHostPatches = Join-Path $sourcePackage.FullName 'host-patches'
        if (-not (Test-Path -LiteralPath $sourceHostPatches -PathType Container)) {
            throw "Missing host patch payload: $sourceHostPatches"
        }
        Copy-Item -LiteralPath $sourceHostPatches -Destination $destinationPackage -Recurse -Force
    }

    $userRoot = Join-Path $destination 'User'
    New-Item -ItemType Directory -Path $userRoot -Force | Out-Null
    $frontendConfig = Join-Path $userRoot 'config.ini'
    if (-not (Test-Path -LiteralPath $frontendConfig -PathType Leaf)) {
        @(
        '[Video]'
        'resolution=1280x720'
        'backend=Vulkan'
        'widescreen_hack=false'
        'aspect_ratio=1'
        'force_texture_filtering=0'
        'max_anisotropy=-1'
        'osd_font_size=16'
        'navigation_overlay=0'
        'native_ultrawide=false'
        'ultrawide_efb_scale=5'
        'text_upscale=false'
        'minimap_high_contrast=false'
        'hires_textures=false'
        'cache_hires_textures=false'
        'dump_textures=false'
        'fullscreen=false'
        'show_fps_in_title=true'
        '[QualityOfLife]'
        'sixty_fps=false'
        'fast_forward=false'
        'fast_forward_multiplier=2'
        ) | Set-Content -LiteralPath $frontendConfig -Encoding ascii
    }

    $gameSettings = Join-Path $userRoot 'GameSettings'
    New-Item -ItemType Directory -Path $gameSettings -Force | Out-Null
    $gameSettingsPath = Join-Path $gameSettings 'GC6E01.ini'
    if (-not (Test-Path -LiteralPath $gameSettingsPath -PathType Leaf)) {
        @'
# Pokemon Colosseum stable 30 FPS profile.
[ActionReplay]
$60 FPS (Nerdzilla)
04005D98 38600000
[ActionReplay_Enabled]
'@ | Set-Content -LiteralPath $gameSettingsPath -Encoding ascii
    }

    # Ship a usable keyboard fallback so first launch does not require an SDL
    # gamepad. Selecting a gamepad in the launcher replaces this profile with
    # ModernGekko's generated SDL mapping.
    $controllerConfig = Join-Path $userRoot 'Config'
    New-Item -ItemType Directory -Path $controllerConfig -Force | Out-Null
    $controllerConfigPath = Join-Path $controllerConfig 'GCPadNew.ini'
    if (-not (Test-Path -LiteralPath $controllerConfigPath -PathType Leaf)) {
        @'
[GCPad1]
Device = DInput/0/Keyboard Mouse
Buttons/A = `X`
Buttons/B = `Z`
Buttons/X = `C`
Buttons/Y = `V`
Buttons/Z = `LSHIFT`
Buttons/Start = `RETURN`
Main Stick/Up = `W`
Main Stick/Down = `S`
Main Stick/Left = `A`
Main Stick/Right = `D`
Main Stick/Calibration = 100.00
C-Stick/Up = `I`
C-Stick/Down = `K`
C-Stick/Left = `J`
C-Stick/Right = `L`
C-Stick/Calibration = 100.00
Triggers/L = `Q`
Triggers/R = `E`
Triggers/L-Analog = `Q`
Triggers/R-Analog = `E`
D-Pad/Up = `UP`
D-Pad/Down = `DOWN`
D-Pad/Left = `LEFT`
D-Pad/Right = `RIGHT`

[GCPad2]
[GCPad3]
[GCPad4]
'@ | Set-Content -LiteralPath $controllerConfigPath -Encoding ascii
    }

    $destinationUri = [Uri]([IO.Path]::GetFullPath($destination).TrimEnd('\') + '\')
    $gameUri = [Uri][IO.Path]::GetFullPath($gameRoot)
    $relativeGame = [Uri]::UnescapeDataString(
        $destinationUri.MakeRelativeUri($gameUri).ToString()
    ).Replace('/', '\')
    Set-Content -LiteralPath (Join-Path $destination 'default-game.txt') `
        -Value $relativeGame -Encoding ascii
}

if (Test-Path -LiteralPath $packageRoot) {
    # A package refresh may replace binaries and bundled mods, but the portable
    # User directory is the player's memory card, settings, and controller
    # profile. Preserve it so rerunning setup never erases normal progression.
    Get-ChildItem -LiteralPath $packageRoot -Force |
        Where-Object Name -ne 'User' |
        Remove-Item -Recurse -Force
}
Install-Package $packageRoot

if ($InstallRepositoryRoot) {
    Install-Package $projectRoot
}

Write-Output "Packaged ImGui frontend (30 FPS by default): $packageRoot"
if ($InstallRepositoryRoot) {
    Write-Output "Installed repository launcher: $(Join-Path $projectRoot 'PokemonColosseumRecomp.exe')"
}

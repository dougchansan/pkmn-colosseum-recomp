param(
    [switch]$InstallRepositoryRoot,
    [switch]$SkipModuleBuild,
    # Builds the private AI gameplay-driving harness into the runtime: the
    # structured agent-state export, the step/intent commands, and the agent and
    # controller overlays. Never pass this for a player release. The harness
    # scripts themselves are run from the repository and are not packaged.
    [switch]$AiHarness,
    # Build a distributable copy under build\dist, from nothing. An ordinary
    # run refreshes build\release and deliberately preserves its User
    # directory, which on a machine that has actually played the game holds a
    # memory card, savestates and that player's own settings -- everything a
    # release must not carry. This mode writes to its own path and never touches
    # build\release, so a distribution build cannot pick up local state and
    # cannot destroy it either.
    [switch]$Distribution,
    # Ship the curated UI and title-screen texture overrides from
    # assets/hd-textures. Off by default: they are replacement game textures
    # and are being held back from the player release for now. The capability
    # is kept rather than deleted so re-enabling is one flag.
    [switch]$HdTextures,
    [string]$CommunityHdPackZip = '',

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
$distributionRoot = Join-Path $projectRoot 'build\dist\PokemonColosseumRecomp'
$gameRoot = Join-Path $projectRoot 'extracted\Pokemon-Colosseum-USA'
$marker = Join-Path $projectRoot 'build\modules\GC6E01\active-module-c.txt'
$module60Root = Join-Path $projectRoot 'b60'
$marker60 = Join-Path $module60Root 'GC6E01\active-module-c.txt'
$dolPatchManifest = Join-Path $projectRoot 'patches\GC6E01-native-widescreen.csv'
# Guest functions replaced by native routines in the GX runtime. Kept beside
# the DOL patches and passed explicitly, because a module built without them
# is correct but materially slower and nothing else would flag the omission.
$nativeSubstManifest = Join-Path $projectRoot 'patches\GC6E01-native-substitutions.csv'
$hdTextureRoot = Join-Path $projectRoot 'assets\hd-textures'

if ($CommunityHdPackZip) {
    $CommunityHdPackZip = [IO.Path]::GetFullPath($CommunityHdPackZip)
    if (-not (Test-Path -LiteralPath $CommunityHdPackZip -PathType Leaf)) {
        throw "Missing community HD texture pack: $CommunityHdPackZip"
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
    '-DMODERNGEKKO_DEFAULT_WINDOW_TITLE=Pokemon Colosseum Recomp' `
    '-DMODERNGEKKO_LOG_FILENAME=PokemonColosseumRecomp.log' `
    "-DMODERNGEKKO_DOL_PATCH_MANIFEST=$dolPatchManifest" `
    -DMODERNGEKKO_PORTABLE_DEFAULT_GAME=ON `
    -DMODERNGEKKO_PORTABLE_USER_DIRECTORY=ON `
    -DMODERNGEKKO_GAMECUBE_CONTROLLERS=ON `
    '-DMODERNGEKKO_NATIVE_ULTRAWIDE_MOD_PACKAGE=Mods/Optional/pokemon-colosseum-ultrawide.mgm' `
    '-DMODERNGEKKO_TEXT_UPSCALE_MOD_PACKAGE=Mods/Optional/pokemon-colosseum-hd-text.mgm' `
    '-DMODERNGEKKO_MINIMAP_MOD_PACKAGE=Mods/Optional/pokemon-colosseum-minimap.mgm' `
    "-DMODERNGEKKO_ENABLE_AI_HARNESS=$(if ($AiHarness) { 'ON' } else { 'OFF' })"
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
    foreach ($substitution in (Import-Csv -LiteralPath $nativeSubstManifest)) {
        $portArguments += @('--native-subst', "$($substitution.guest):$($substitution.symbol)")
    }
    & $port @portArguments
    if ($LASTEXITCODE -ne 0) {
        throw 'Stable C module build failed.'
    }

    # No extra --dol-patch here. This variant used to bake the 60 FPS Action
    # Replay instruction (80005D98=38600000) into the module, but that
    # instruction is never executed -- the function containing it is superseded
    # during boot -- and the frame period is set host-side instead, by
    # SetColosseum60FpsEnabled. The dead patch was not harmless: the runner
    # patches the player's DOL from GC6E01-native-widescreen.csv, which does not
    # contain 80005D98, so guest RAM kept the original 886D85B0 while the module
    # held 38600000. The static recompiler's hash guard saw the mismatch and ran
    # chunk [0x800055E0,0x800095E0) in the interpreter for the whole session,
    # every time a player enabled 60 FPS.
    $port60Arguments = @($portArguments)
    $port60Arguments[$port60Arguments.IndexOf($moduleOutput)] = $module60Root
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
$canonicalHostPatches = Join-Path $projectRoot 'mods\host-patches'
foreach ($required in @($launcher, $runner, $sys, $patches, $modPackages,
        $canonicalHostPatches)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Missing package input: $required"
    }
}

function Install-Package([string]$destination, [switch]$DeveloperInstall) {
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Copy-Item -LiteralPath $launcher -Destination (Join-Path $destination 'PokemonColosseumRecomp.exe') -Force
    Copy-Item -LiteralPath $runner -Destination (Join-Path $destination 'moderngekko-run.exe') -Force
    Copy-Item -LiteralPath $modulePath -Destination (Join-Path $destination 'gGC6E01_recomp.dll') -Force
    Copy-Item -LiteralPath $module60Path `
        -Destination (Join-Path $destination 'gGC6E01_recomp_60fps.dll') -Force
    Copy-Item -LiteralPath $sys -Destination $destination -Recurse -Force
    Copy-Item -LiteralPath $patches -Destination $destination -Recurse -Force
    foreach ($releaseDocument in @('LICENSE', 'THIRD_PARTY_NOTICES.md', 'VERSION')) {
        $sourceDocument = Join-Path $projectRoot $releaseDocument
        $destinationDocument = Join-Path $destination $releaseDocument
        if ([IO.Path]::GetFullPath($sourceDocument) -ne
            [IO.Path]::GetFullPath($destinationDocument)) {
            Copy-Item -LiteralPath $sourceDocument -Destination $destination -Force
        }
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
        # Every optional mod needs the same host runtime support. Package the
        # canonical payload so generated mod directories cannot ship stale
        # renderer patches after an incremental build.
        Copy-Item -LiteralPath $canonicalHostPatches -Destination $destinationPackage -Recurse -Force
    }

    $userRoot = Join-Path $destination 'User'
    New-Item -ItemType Directory -Path $userRoot -Force | Out-Null
    if ($CommunityHdPackZip) {
        $resourcePacks = Join-Path $userRoot 'ResourcePacks'
        New-Item -ItemType Directory -Path $resourcePacks -Force | Out-Null
        Copy-Item -LiteralPath $CommunityHdPackZip `
            -Destination (Join-Path $resourcePacks 'Pokemon Colosseum HD Pack.zip') -Force
    }
    # Load/Textures is created either way: it is where a player drops the
    # community HD pack, and the runtime expects the directory to exist.
    $textureDestination = Join-Path $userRoot 'Load\Textures'
    New-Item -ItemType Directory -Path $textureDestination -Force | Out-Null
    $hdTextureSource = Join-Path $hdTextureRoot 'GC6E01'
    if (-not $HdTextures) {
        Write-Output ('Packaging without the curated texture overrides; ' +
            'pass -HdTextures to include them.')
    } elseif (Test-Path -LiteralPath $hdTextureSource -PathType Container) {
        Copy-Item -LiteralPath $hdTextureSource -Destination $textureDestination `
            -Recurse -Force
    } else {
        Write-Warning ("No HD textures at $hdTextureSource; packaging without " +
            'them. hires_textures stays on, so a pack dropped in later is used.')
    }
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
        'ultrawide_efb_scale=5'
        'text_upscale=false'
        'minimap_high_contrast=false'
        'hires_textures=true'
        'community_hd_texture_pack=false'
        'cache_hires_textures=false'
        'dump_textures=false'
        'fullscreen=false'
        'show_fps_in_title=true'
        '[QualityOfLife]'
        'sixty_fps=false'
        'guest_idle_skip=true'
        'fast_forward=false'
        'fast_forward_multiplier=2'
        ) | Set-Content -LiteralPath $frontendConfig -Encoding ascii
    }
    # This package includes a narrowly scoped replacement for the title-logo
    # hash. Ensure upgrades activate it without enabling the retired blanket
    # texture dump, which remains outside User/Load/Textures.
    $frontendConfigText = Get-Content -LiteralPath $frontendConfig -Raw
    $frontendConfigText = $frontendConfigText -replace `
        '(?m)^hires_textures=false\r?$', 'hires_textures=true'
    if ($frontendConfigText -notmatch '(?m)^guest_idle_skip=') {
        $frontendConfigText = $frontendConfigText -replace `
            '(?m)^(sixty_fps=)', "guest_idle_skip=true`r`n`$1"
    }
    if ($frontendConfigText -notmatch '(?m)^community_hd_texture_pack=') {
        $frontendConfigText = $frontendConfigText -replace `
            '(?m)^(cache_hires_textures=)', "community_hd_texture_pack=false`r`n`$1"
    }
    Set-Content -LiteralPath $frontendConfig -Value $frontendConfigText -Encoding ascii

    $gameSettings = Join-Path $userRoot 'GameSettings'
    New-Item -ItemType Directory -Path $gameSettings -Force | Out-Null
    $gameSettingsPath = Join-Path $gameSettings 'GC6E01.ini'
    if (-not (Test-Path -LiteralPath $gameSettingsPath -PathType Leaf)) {
        @'
# Pokemon Colosseum stable 30 FPS profile.
[Core]
# The task-list-aware idle trigger. The runtime carries the code but it is inert
# until these three game-specific keys name the loop, and without them battle-mid
# runs at 55 FPS instead of holding 60.
StaticRecompTaskIdlePC = 0x800FEB9C
StaticRecompTaskListHead = 0x8047AC98
StaticRecompTaskPendingHead = 0x8047AC9C

[ActionReplay]
$60 FPS (Nerdzilla)
04005D98 38600000
[ActionReplay_Enabled]
'@ | Set-Content -LiteralPath $gameSettingsPath -Encoding ascii
    }

    # These three keys are now owned by the runtime, which rewrites them on every
    # boot from the launcher's Guest idle skip toggle. They are still written
    # here so a fresh package carries them before its first launch.
    # User/GameSettings survives a package refresh, so an install made before the
    # idle trigger existed keeps its old GC6E01.ini and would never see the keys.
    # Add them in place rather than defaulting them, and leave an explicit value
    # alone so a measurement run can still override one.
    $gameSettingsText = Get-Content -LiteralPath $gameSettingsPath -Raw
    if ($gameSettingsText -notmatch '(?m)^\s*StaticRecompTaskIdlePC\s*=') {
        $taskIdleKeys = @(
            'StaticRecompTaskIdlePC = 0x800FEB9C'
            'StaticRecompTaskListHead = 0x8047AC98'
            'StaticRecompTaskPendingHead = 0x8047AC9C'
        ) -join "`r`n"
        if ($gameSettingsText -match '(?m)^\[Core\][ \t]*$') {
            $gameSettingsText = [regex]::Replace(
                $gameSettingsText, '(?m)^\[Core\][ \t]*\r?$', "[Core]`r`n$taskIdleKeys", 1)
        } else {
            $gameSettingsText = "[Core]`r`n$taskIdleKeys`r`n`r`n" + $gameSettingsText
        }
        Set-Content -LiteralPath $gameSettingsPath -Value $gameSettingsText -Encoding ascii
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

    # Only the in-repo developer install gets a default game. The relative path
    # this computes escapes the package root ('..\..\..\extracted\...'), so shipping
    # it to a player names a directory that does not exist on their machine and
    # leaks the build tree's layout. With no file the launcher opens on its disc
    # picker, which is the correct first run.
    $defaultGameFile = Join-Path $destination 'default-game.txt'
    if ($DeveloperInstall) {
        $destinationUri = [Uri]([IO.Path]::GetFullPath($destination).TrimEnd('\') + '\')
        $gameUri = [Uri][IO.Path]::GetFullPath($gameRoot)
        $relativeGame = [Uri]::UnescapeDataString(
            $destinationUri.MakeRelativeUri($gameUri).ToString()
        ).Replace('/', '\')
        Set-Content -LiteralPath $defaultGameFile -Value $relativeGame -Encoding ascii
    } elseif (Test-Path -LiteralPath $defaultGameFile -PathType Leaf) {
        Remove-Item -LiteralPath $defaultGameFile -Force
    }
}

if ($Distribution) {
    # Generated wholesale every time, User directory included, so the result is
    # whatever Install-Package writes from its templates and nothing else.
    if (Test-Path -LiteralPath $distributionRoot) {
        Remove-Item -LiteralPath $distributionRoot -Recurse -Force
    }
    Install-Package $distributionRoot

    # A distribution carrying save data or savestates is the failure this mode
    # exists to prevent, so check rather than trust. Install-Package writes only
    # config.ini, GameSettings, Config and Load/Textures.
    $userRoot = Join-Path $distributionRoot 'User'
    $allowed = @('config.ini', 'GameSettings', 'Config', 'Load', 'ResourcePacks')
    $unexpected = Get-ChildItem -LiteralPath $userRoot -Force |
        Where-Object { $_.Name -notin $allowed }
    if ($unexpected) {
        throw ('Distribution User directory has unexpected entries: ' +
               ($unexpected.Name -join ', '))
    }
    foreach ($pattern in @('*.gci', '*.raw', '*.sav', '*.s01')) {
        $leaked = Get-ChildItem -LiteralPath $userRoot -Recurse -File -Filter $pattern `
            -ErrorAction SilentlyContinue
        if ($leaked) {
            throw ('Distribution carries player data: ' +
                   (($leaked | Select-Object -First 5).Name -join ', '))
        }
    }
} else {
    if (Test-Path -LiteralPath $packageRoot) {
        # A package refresh may replace binaries and bundled mods, but the
        # portable User directory is the player's memory card, settings, and
        # controller profile. Preserve it so rerunning setup never erases normal
        # progression.
        Get-ChildItem -LiteralPath $packageRoot -Force |
            Where-Object Name -ne 'User' |
            Remove-Item -Recurse -Force
    }
    Install-Package $packageRoot

    if ($InstallRepositoryRoot) {
        Install-Package $projectRoot -DeveloperInstall
    }
}

if ($AiHarness) {
    Write-Warning 'This package contains the private AI harness. Do not publish it as a player release.'
}
if ($Distribution) {
    Write-Output "Packaged distribution (30 FPS by default): $distributionRoot"
    Write-Output 'User directory generated from templates; no local state carried.'
    return
}
Write-Output "Packaged ImGui frontend (30 FPS by default): $packageRoot"
if ($InstallRepositoryRoot) {
    Write-Output "Installed repository launcher: $(Join-Path $projectRoot 'PokemonColosseumRecomp.exe')"
}

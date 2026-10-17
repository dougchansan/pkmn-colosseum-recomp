param(
    [Parameter(Mandatory)][string]$PerformanceDirectory,
    [string]$UserDirectory,
    [ValidateRange(1, 3600)][int]$Seconds = 60,
    [string]$Output
)

$ErrorActionPreference = 'Stop'
$statusPath = Join-Path $PerformanceDirectory 'status.txt'
if (-not (Test-Path -LiteralPath $statusPath)) { throw "No runtime status at $statusPath. Launch with -PerformanceDirectory first." }
if (-not $UserDirectory) { $UserDirectory = Join-Path (Split-Path -Parent $PSScriptRoot) 'build/freecam-user' }
if (-not $Output) { $Output = Join-Path $PerformanceDirectory ('performance-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.csv') }

function Read-KeyValues([string]$Path) {
    $values = @{}
    if (Test-Path -LiteralPath $Path) {
        foreach ($line in Get-Content -LiteralPath $Path) {
            if ($line -match '^\s*([^#;\[=]+?)\s*=\s*(.*?)\s*$') { $values[$Matches[1]] = $Matches[2] }
        }
    }
    return $values
}

$samples = for ($sample = 0; $sample -lt $Seconds; $sample++) {
    $status = Read-KeyValues $statusPath
    $settings = Read-KeyValues (Join-Path $UserDirectory 'config.ini')
    [pscustomobject]@{
        Time = (Get-Date).ToString('o')
        State = $status.state
        FPS = $status.fps
        VPS = $status.vps
        Speed = $status.speed
        FrameCount = $status.frame_count
        Room = $status.navigation_archive
        ResolutionSetting = $settings.resolution
        MenuScaleSetting = $settings.ResolutionScale
        HDTexturesSetting = $settings.hires_textures
    }
    Start-Sleep -Seconds 1
}
$samples | Export-Csv -LiteralPath $Output -NoTypeInformation
Write-Output "Saved $($samples.Count) samples to $Output"

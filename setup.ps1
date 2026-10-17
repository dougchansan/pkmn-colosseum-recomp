[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Iso,

    [ValidateRange(1, 16)]
    [int]$ParallelJobs = 4,

    # Skip downloading the HD texture packs.
    [switch]$NoHdPacks
)

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'scripts\setup-windows.ps1') `
    -Iso $Iso -ParallelJobs $ParallelJobs -NoHdPacks:$NoHdPacks

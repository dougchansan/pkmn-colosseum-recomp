[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ModernGekkoSource,

    [string]$RecompCoreSource,

    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'
$bundleRoot = $PSScriptRoot
$manifestPath = Join-Path $bundleRoot 'manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json

function Invoke-Git {
    param(
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$AllowFailure
    )

    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & git -C $Repository @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    if (-not $AllowFailure -and $exitCode -ne 0) {
        throw "git -C '$Repository' $($Arguments -join ' ') failed:`n$($output -join "`n")"
    }
    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = $output
    }
}

function Resolve-Repository {
    param([Parameter(Mandatory = $true)][string]$Path)

    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $probe = Invoke-Git -Repository $resolved -Arguments @('rev-parse', '--is-inside-work-tree') -AllowFailure
    if ($probe.ExitCode -ne 0 -or ($probe.Output -join '').Trim() -ne 'true') {
        throw "Not a Git working tree: $resolved"
    }
    return $resolved
}

function Test-CleanTrackedFiles {
    param(
        [Parameter(Mandatory = $true)][string]$Repository,
        [switch]$IgnoreSubmodules
    )

    $arguments = @('status', '--porcelain', '--untracked-files=no')
    if ($IgnoreSubmodules) {
        $arguments += '--ignore-submodules=dirty'
    }
    $status = Invoke-Git -Repository $Repository -Arguments $arguments
    if (($status.Output -join '').Trim()) {
        throw "Tracked files are not clean in $Repository. Commit or stash them before applying host patches."
    }
}

function Test-CompatibleBase {
    param(
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][string]$BaseCommit,
        [Parameter(Mandatory = $true)][string]$Component
    )

    $known = Invoke-Git -Repository $Repository -Arguments @('cat-file', '-e', "$BaseCommit^{commit}") -AllowFailure
    if ($known.ExitCode -ne 0) {
        throw "$Component checkout does not contain required upstream base $BaseCommit."
    }
    $ancestor = Invoke-Git -Repository $Repository -Arguments @('merge-base', '--is-ancestor', $BaseCommit, 'HEAD') -AllowFailure
    if ($ancestor.ExitCode -ne 0) {
        throw "$Component HEAD is not based on required upstream revision $BaseCommit. Update from the ref recorded in manifest.json."
    }
}

function Invoke-Patch {
    param(
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][string]$PatchPath,
        [Parameter(Mandatory = $true)][string]$ExpectedSha256,
        [Parameter(Mandatory = $true)][string]$Component,
        [switch]$IgnoreSubmodules
    )

    $actualSha256 = (Get-FileHash -LiteralPath $PatchPath -Algorithm SHA256).Hash
    if ($actualSha256 -ne $ExpectedSha256) {
        throw "$Component patch checksum mismatch. Expected $ExpectedSha256, got $actualSha256."
    }

    $reverse = Invoke-Git -Repository $Repository -Arguments @('apply', '--reverse', '--check', $PatchPath) -AllowFailure
    if ($reverse.ExitCode -eq 0) {
        Write-Output "$Component host patch is already applied."
        return
    }

    Test-CleanTrackedFiles -Repository $Repository -IgnoreSubmodules:$IgnoreSubmodules
    $check = Invoke-Git -Repository $Repository -Arguments @('apply', '--check', $PatchPath) -AllowFailure
    if ($check.ExitCode -ne 0) {
        throw "$Component host patch does not apply cleanly:`n$($check.Output -join "`n")"
    }
    if ($CheckOnly) {
        Write-Output "$Component host patch applies cleanly."
        return
    }

    Invoke-Git -Repository $Repository -Arguments @('apply', $PatchPath) | Out-Null
    Write-Output "Applied $Component host patch."
}

$modernGekko = Resolve-Repository -Path $ModernGekkoSource
if (-not $RecompCoreSource) {
    $RecompCoreSource = Join-Path $modernGekko $manifest.recompCore.relativePath
}
$recompCore = Resolve-Repository -Path $RecompCoreSource

Test-CompatibleBase -Repository $recompCore -BaseCommit $manifest.recompCore.baseCommit -Component 'RecompCore'
Test-CompatibleBase -Repository $modernGekko -BaseCommit $manifest.modernGekko.baseCommit -Component 'ModernGekko'

Invoke-Patch -Repository $recompCore -PatchPath (Join-Path $bundleRoot $manifest.recompCore.patch) -ExpectedSha256 $manifest.recompCore.sha256 -Component 'RecompCore'
Invoke-Patch -Repository $modernGekko -PatchPath (Join-Path $bundleRoot $manifest.modernGekko.patch) -ExpectedSha256 $manifest.modernGekko.sha256 -Component 'ModernGekko' -IgnoreSubmodules

if ($CheckOnly) {
    Write-Output 'Host patch compatibility check passed; no files were changed.'
} else {
    Write-Output 'Colosseum host support is ready to build.'
}

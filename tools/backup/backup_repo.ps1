<#
.SYNOPSIS
    Packs the whole repository history into a single self-contained .bundle file.

.DESCRIPTION
    A bundle holds every commit, branch and tag in one file. `git clone` accepts
    it directly, so one file is enough to bring the project back on any host,
    with authorship and history intact, without a server.

    Uncommitted work is picked up too: `git stash create` turns the working tree
    into a commit object, which is stored under refs/backup/wip/<stamp> inside
    the bundle. The temporary ref is deleted from the repository afterwards, so
    nothing is left behind; the object survives inside the bundle.

    The bundle is verified after it is written. Older bundles beyond -Keep are
    pruned, newest first.

.PARAMETER Dest
    Directory the bundles are written to. Defaults to a larecomp-backups folder
    next to the repository, so backups never land inside the working tree.

.PARAMETER Keep
    How many bundles to keep in -Dest. Defaults to 10, 0 keeps everything.

.PARAMETER SkipDirty
    Bundle committed history only, leaving uncommitted work out.

.EXAMPLE
    .\backup_repo.ps1

.EXAMPLE
    .\backup_repo.ps1 -Dest E:\backups\larecomp -Keep 20
#>
[CmdletBinding()]
param(
    [string]$Dest,
    [int]$Keep = 10,
    [switch]$SkipDirty
)

$ErrorActionPreference = 'Stop'

$repo = (git -C $PSScriptRoot rev-parse --show-toplevel).Trim()
if (-not $repo) { throw "not inside a git repository: $PSScriptRoot" }

if (-not $Dest) {
    $Dest = Join-Path (Split-Path (Resolve-Path $repo) -Parent) 'larecomp-backups'
}
if (-not (Test-Path $Dest)) { New-Item -ItemType Directory -Path $Dest | Out-Null }

$stamp = Get-Date -Format 'yyyyMMdd-HHmm'
$head = (git -C $repo rev-parse --short HEAD).Trim()
$name = "larecomp-$stamp-$head"
$bundle = Join-Path $Dest "$name.bundle"

# Uncommitted work becomes a commit object under a temporary ref, so --all
# sweeps it into the bundle along with the branches. This is built against a
# throwaway index rather than `git stash create`, which only sees tracked files
# and would leave new ones out of the backup. The real index is never touched.
$wipRef = $null
if (-not $SkipDirty) {
    $tmpIndex = Join-Path ([IO.Path]::GetTempPath()) "larecomp-backup-index-$stamp"
    $env:GIT_INDEX_FILE = $tmpIndex
    try {
        git -C $repo read-tree HEAD
        git -C $repo add -A
        $tree = (git -C $repo write-tree).Trim()
        if ($tree -ne (git -C $repo rev-parse 'HEAD^{tree}').Trim()) {
            $wip = (git -C $repo commit-tree $tree -p HEAD -m "working tree at $stamp").Trim()
            $wipRef = "refs/backup/wip/$stamp"
            git -C $repo update-ref -m "backup_repo.ps1: working tree at $stamp" $wipRef $wip
            Write-Host "working tree captured as $wipRef"
        } else {
            Write-Host "working tree clean, nothing extra to capture"
        }
    } finally {
        $env:GIT_INDEX_FILE = $null
        if (Test-Path $tmpIndex) { Remove-Item $tmpIndex -Force }
    }
}

try {
    git -C $repo bundle create $bundle --all
} finally {
    if ($wipRef) { git -C $repo update-ref -d $wipRef }
}

git -C $repo bundle verify $bundle | Out-Null
if ($LASTEXITCODE -ne 0) { throw "bundle failed verification: $bundle" }

# Sidecar listing, so the contents of a bundle can be read without git.
$manifest = Join-Path $Dest "$name.txt"
@(
    "repository : $repo"
    "written    : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss K')"
    "head       : $(git -C $repo log -1 --format='%H %s' HEAD)"
    ""
    "refs:"
    (git -C $repo bundle list-heads $bundle)
) | Set-Content -Path $manifest -Encoding UTF8

$size = [math]::Round((Get-Item $bundle).Length / 1MB, 2)
Write-Host ""
Write-Host "bundle  : $bundle ($size MB)"
Write-Host "manifest: $manifest"

if ($Keep -gt 0) {
    $old = Get-ChildItem -Path $Dest -Filter 'larecomp-*.bundle' |
        Sort-Object LastWriteTime -Descending | Select-Object -Skip $Keep
    foreach ($f in $old) {
        Remove-Item $f.FullName -Force
        $txt = [IO.Path]::ChangeExtension($f.FullName, '.txt')
        if (Test-Path $txt) { Remove-Item $txt -Force }
        Write-Host "pruned  : $($f.Name)"
    }
}

Write-Host ""
Write-Host "restore with:"
Write-Host "    git clone `"$bundle`" larecomp-restored"

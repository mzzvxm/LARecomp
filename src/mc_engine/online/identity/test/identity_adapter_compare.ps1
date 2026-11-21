# Compares the game's identity code before and after it was split into a core (identity_core, identity_net) and a thin
# game layer (identity_store, identity_client). identity_adapter_test.cpp compiles the game's own identity_store.cpp and
# identity_client.cpp without the SDK and exercises them through their public API; this script builds it twice, once
# against the tree as it was at -OldRef and once against this tree, runs every scenario on both, and compares what they
# print: the checks (all must pass on both) and the lines the code logged (must be identical).
#
#   powershell -NoProfile -File identity_adapter_compare.ps1 [-OldRef 4364d9ca] [-Clang <clang++.exe>] [-Work <folder>]
#
# Needs git and tar on the PATH and a clang++ that targets Windows (the repo's labuild\tools\clang+llvm-... is the
# default). No game, no SDK, no real server: the only network is 127.0.0.1. Exit code 0 = both passed and agree.

param(
    [string]$OldRef = '4364d9ca',
    [string]$Clang = 'C:\mcv\larecomp\labuild\tools\clang+llvm-20.1.8-x86_64-pc-windows-msvc\bin\clang++.exe',
    [string]$Work = (Join-Path $env:TEMP 'identity-adapter-compare')
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..\..')).Path
$scenarios = 'fresh', 'existing', 'unreadable', 'legacy', 'legacyxuid', 'ops', 'client'

if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
$oldTree = Join-Path $Work 'old'
New-Item -ItemType Directory -Force $oldTree | Out-Null

# The tree as it was, from git (nothing in the working tree is touched).
$tarFile = Join-Path $Work 'old.tar'
& git -C $repo archive --format=tar -o $tarFile $OldRef src/mc_engine/online thirdparty/monocypher src/larecomp_version.h
if ($LASTEXITCODE -ne 0) { throw "git archive failed for $OldRef" }
& tar -xf $tarFile -C $oldTree
if ($LASTEXITCODE -ne 0) { throw 'tar failed' }
# The check itself (and its stand-ins) comes from this tree: it is the same program on both sides.
Copy-Item -Recurse -Force (Join-Path $repo 'src\mc_engine\online\identity\test') (Join-Path $oldTree 'src\mc_engine\online\identity\')

function Build-Variant([string]$root, [string]$exe, [bool]$withCore) {
    $idDir = Join-Path $root 'src\mc_engine\online\identity'
    $online = Join-Path $root 'src\mc_engine\online'
    $files = @(
        (Join-Path $idDir 'test\identity_adapter_test.cpp'),
        (Join-Path $idDir 'identity_store.cpp'),
        (Join-Path $idDir 'identity_client.cpp')
    )
    if ($withCore) {
        $files += (Join-Path $idDir 'identity_core.cpp')
        $files += (Join-Path $idDir 'identity_net.cpp')
    }
    $files += (Join-Path $online 'relay_info_config.cpp')
    $files += (Join-Path $online 'relay_info.cpp')
    $files += (Join-Path $online 'rate_my_ride\rmr_net.cpp')
    $files += (Join-Path $online 'master_server\identity_registry.cpp')
    $args2 = @('-std=c++23', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-I', (Join-Path $idDir 'test\stubs')) + $files +
        @('-x', 'c++', (Join-Path $root 'thirdparty\monocypher\monocypher.c'), '-lws2_32', '-lcrypt32', '-o', $exe)
    & $Clang @args2
    if ($LASTEXITCODE -ne 0) { throw "build failed: $exe" }
}

$oldExe = Join-Path $Work 'adapter_before.exe'
$newExe = Join-Path $Work 'adapter_after.exe'
Build-Variant $oldTree $oldExe $false
Build-Variant $repo $newExe $true

function Get-Normalized([string]$exe, [string]$scenario) {
    $lines = & $exe $scenario 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $text = $lines | Where-Object { $_ -notmatch '^identity: ' } |
        ForEach-Object { ($_ -replace '127\.0\.0\.1:\d+', '127.0.0.1:<port>') -replace 'key [0-9a-f]{10} XUID [0-9A-F]{16}', 'key <fingerprint> XUID <xuid>' }
    return [pscustomobject]@{ Code = $code; Text = $text }
}

$bad = 0
foreach ($sc in $scenarios) {
    $before = Get-Normalized $oldExe $sc
    $after = Get-Normalized $newExe $sc
    $same = (($before.Text -join "`n") -eq ($after.Text -join "`n"))
    $summary = ($after.Text | Select-Object -Last 1)
    if ($before.Code -ne 0 -or $after.Code -ne 0 -or -not $same) {
        $bad++
        Write-Host ("{0}: DIFFERENT or failed (before exit {1}, after exit {2})" -f $sc, $before.Code, $after.Code)
        Compare-Object $before.Text $after.Text | Select-Object -First 10 | Out-Host
    } else {
        Write-Host ("{0}: same checks and same log lines ({1})" -f $sc, $summary)
    }
}
if ($bad -gt 0) { Write-Host "$bad scenario(s) differ"; exit 1 }
Write-Host 'The identity code behaves the same before and after the split.'
exit 0

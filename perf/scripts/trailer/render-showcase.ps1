[CmdletBinding()]
param(
    [switch]$Plan,
    [ValidateSet(1, 2)][int]$Round = 1,
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,59}$')][string]$RunTag = 'trailer-showcase-r1',
    [string]$ExeDir = 'ports/exe/director-preset-track',
    [string]$PackDir = 'ports/scratch/packs-director-preset-track'
)

$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path "$PSScriptRoot/../../..").Path
if (!(Test-Path -LiteralPath "$workspace/ports/scripts/opt_run.py")) {
    $workspace = (Resolve-Path "$PSScriptRoot/../../../../..").Path
}
. "$workspace/perf/scripts/lockrun-lease.ps1"
$lockScript = Find-LockrunPy
if (!$lockScript) { throw 'lockrun.py is required.' }
if (!$Plan -and (Test-UnderLockrun)) { throw 'This runner acquires its own game lease.' }

Push-Location -LiteralPath $workspace
try {
    $interpreter = (Get-Command python).Source
    $ffmpegPath = (Get-Command ffmpeg -CommandType Application | Select-Object -First 1).Source
    $configPath = Join-Path $PSScriptRoot 'showcase.json'
    $showcase = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
    $runDir = Join-Path $workspace "ports/run/$RunTag"
    $exeFolder = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ExeDir)
    $packFolder = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($PackDir)
    $launchArgs = @($interpreter, '-I', '-B', $lockScript, '--lock', '--keep-display', '--game',
        '--max-min', '90', '--log', (Join-Path $workspace "perf/logs/lockrun-$RunTag.log"), '--',
        $interpreter, '-I', '-B', (Join-Path $workspace 'ports/scripts/opt_run.py'),
        '--name', $RunTag, '--exe-dir', $exeFolder, '--backend', 'd3d12',
        '--adapter-name', 'NVIDIA GeForce RTX 5080', '--window', '3840x2160',
        '--scale', '0', '--msaa', '1', '--no-pm', '--log-level', 'info',
        '--fps-limit', '60', '--save', 'mzlate', '--starter', 'ref', '--use-game-preset',
        '--own-exe-dir', '--director', '--roam-s', '5150', '--max-run-s', '5300',
        '--pack', (Join-Path $packFolder 'mcla_shaders.v8.pack'), '--extra',
        "--shader_pack_dir=$packFolder", "--mcla_capture_dir=$runDir/takes",
        "--mcla_capture_ffmpeg=$ffmpegPath",
        '--mcla_scene_seed=1', '--mcla_scene_seed_each_frame=true', '--mcla_capture_statelog=true',
        '--mcla_gpu_preset=custom', '--mcla_gpu_upscaler=off', '--mcla_gpu_frame_generation=off',
        '--mcla_gpu_hdr=off', '--fullscreen=true', '--fullscreen_exclusive=false')
    $row = @{ argv = $launchArgs; use = 'other'; counters = $false; channel = (Join-Path $runDir 'director.json') }
    if ($Plan) {
        Write-Output "QUESTION: $($showcase.question)"
        Write-Output "ITEM: $($showcase.item)"
        Write-Output "ROUND: $Round"
        Write-Output "DECISION: $($showcase.decision)"
        Write-Output "EXPIRES: $($showcase.expires)"
        foreach ($shot in $showcase.shots) {
            $sceneFile = Join-Path $PSScriptRoot "scenes/$($shot.id).scene"
            if (!(Test-Path -LiteralPath $sceneFile -PathType Leaf)) { throw "Missing scene $sceneFile." }
            $sceneArgs = @{ scene = $sceneFile; name = $shot.id; from = 0; to = $shot.seconds
                fps = $showcase.fps; sink = 'png'; presets = $showcase.presets }
            Write-Output ('TAKE ' + ($sceneArgs | ConvertTo-Json -Compress))
        }
        Write-Output ('PREFLIGHT ' + ($row | ConvertTo-Json -Depth 5 -Compress))
        return
    }
    if ([DateTimeOffset]::UtcNow -ge [DateTimeOffset]::Parse($showcase.expires)) {
        throw "Showcase dispatch expired at $($showcase.expires). Review the proof before updating it."
    }
    foreach ($cityFile in @('mcla_city.v1.pack', 'mcla_city_hit.v1.pack', 'mcla_city_tex.v1.pack')) {
        if (!(Test-Path -LiteralPath (Join-Path $packFolder "city/$cityFile") -PathType Leaf)) {
            throw "Stage city/$cityFile in $packFolder before rendering."
        }
    }
    # // CHECK: Borderless must produce physical 3840x2160 on the capture desktop.
    $row | ConvertTo-Json -Depth 5 -Compress |
        & $interpreter -I -B "$PSScriptRoot/render_showcase.py" --workspace $workspace --run-tag $RunTag --round $Round --ffmpeg $ffmpegPath
    exit $LASTEXITCODE
} finally {
    Pop-Location
}

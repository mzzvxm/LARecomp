# profile_gpu_commands.ps1
# Runs LARecomp with the guest profiler locked onto the "GPU Commands" thread.
# Slow frame threshold is set to 7.0 ms (targeting 144 FPS / 6.94 ms budget).

$env:MCLA_PROFILE = "1"
$env:MCLA_PROFILE_THREAD = "GPU Commands"
$env:MCLA_PROFILE_SLOW_MS = "7.0"

$BuildDir = Join-Path $PSScriptRoot "out\build\win-amd64-relwithdebinfo"
if (-not (Test-Path $BuildDir)) {
    Write-Error "Build directory not found at $BuildDir"
    exit 1
}

Set-Location -Path $BuildDir
Write-Host "Launching larecomp.exe with GPU Commands profiling (slow frame >= 7.0ms)..." -ForegroundColor Green
.\larecomp.exe

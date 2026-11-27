[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Manifest,
    [string]$OutDir,
    [string]$Ffmpeg = 'ffmpeg',
    [string]$Font = "$env:WINDIR/Fonts/segoeuib.ttf"
)

$ErrorActionPreference = 'Stop'
$manifestPath = (Resolve-Path -LiteralPath $Manifest).Path
if (!$OutDir) { $OutDir = Join-Path (Split-Path -Parent $manifestPath) 'showcase-edit' }
$interpreter = (Get-Command python).Source
$ffmpegPath = (Get-Command -Name $Ffmpeg -CommandType Application | Select-Object -First 1).Source
& $interpreter -I -B "$PSScriptRoot/wipe_showcase.py" --manifest $manifestPath --out $OutDir --ffmpeg $ffmpegPath --font $Font
exit $LASTEXITCODE

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$packaged = Join-Path $projectRoot 'dist\Audiloquy\Audiloquy.exe'

if (-not (Test-Path -LiteralPath $packaged)) {
    & (Join-Path $PSScriptRoot 'build-prototype.ps1')
}

Start-Process -FilePath $packaged -WorkingDirectory $projectRoot

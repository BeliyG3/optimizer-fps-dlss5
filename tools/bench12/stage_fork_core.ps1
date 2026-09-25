param([Parameter(Mandatory = $true)][string] $Runtime,
      [string] $Build = '')
$ErrorActionPreference = 'Stop'
if (-not $Build) { $Build = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\out\build\x64')) }
. "$PSScriptRoot\..\Runtime-Payload.ps1"
$core = Join-Path $Build 'core\Release\optimizer-fps-dlss5-core.dll'
$shaders = Join-Path $Build 'shaders'
Copy-CoreShaders $core $shaders $Runtime

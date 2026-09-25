#Requires -Version 5.1
# Shared staging for bench and CI runtime payloads (PowerShell 5.1).
function Get-CorePayload([string] $Build) {
    $files = @{
        'optimizer-fps-dlss5.addon64' = Join-Path $Build 'hosts\reshade\Release\optimizer-fps-dlss5.addon64'
        'optimizer-fps-dlss5-core.dll' = Join-Path $Build 'core\Release\optimizer-fps-dlss5-core.dll'
        'nvngx.dll_optimizerfps.dll' = Join-Path $Build 'hosts\reshade\ngx_forwarder\Release\nvngx.dll_optimizerfps.dll'
    }
    foreach ($source in $files.Values) {
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing payload: $source" }
    }
    return $files
}

function Copy-CoreShaders([string] $Core, [string] $Shaders, [string] $Runtime) {
    $versionText = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '..\cmake\Version.cmake'))
    $version = [regex]::Match($versionText, 'OFPS_RELEASE_VERSION\s+"([^"]+)"').Groups[1].Value
    if (-not $version -or (Get-Item -LiteralPath $Core).VersionInfo.FileVersion -ne $version) {
        throw "Core DLL version does not match $version : $Core"
    }
    $built = @(Get-ChildItem -LiteralPath $Shaders -File -Filter '*.dxbc')
    if ($built.Count -eq 0) { throw "No compiled DXBC: $Shaders" }
    $builtNames = @($built | ForEach-Object { $_.Name.ToLowerInvariant() } | Sort-Object)
    if (@($builtNames | Select-Object -Unique).Count -ne $built.Count) {
        throw "Duplicate DXBC names in build output: $Shaders"
    }
    $root = [IO.Path]::GetFullPath($Runtime).TrimEnd('\')
    $destination = [IO.Path]::GetFullPath((Join-Path $root 'optimizer-fps-dlss5'))
    if (-not $destination.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Shader destination escapes runtime: $destination"
    }
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    foreach ($directory in @($root, $destination)) {
        if ((Get-Item -LiteralPath $directory).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Runtime staging rejects reparse points: $directory"
        }
    }
    Copy-Item -LiteralPath $Core -Destination (Join-Path $root 'optimizer-fps-dlss5-core.dll') -Force
    Get-ChildItem -LiteralPath $destination -File -Filter '*.dxbc' | Remove-Item -Force
    $built | Copy-Item -Destination $destination -Force
    $stagedNames = @(Get-ChildItem -LiteralPath $destination -File -Filter '*.dxbc' |
        ForEach-Object { $_.Name.ToLowerInvariant() } | Sort-Object)
    if ($stagedNames.Count -ne $builtNames.Count -or
        @(Compare-Object $builtNames $stagedNames).Count -ne 0) {
        throw "Shader staging differs from build output: $destination"
    }
}

function Copy-CorePayload([string] $Build, [string] $Runtime) {
    $files = Get-CorePayload $Build
    Copy-CoreShaders $files['optimizer-fps-dlss5-core.dll'] (Join-Path $Build 'shaders') $Runtime
    foreach ($name in @('optimizer-fps-dlss5.addon64', 'nvngx.dll_optimizerfps.dll')) {
        Copy-Item -LiteralPath $files[$name] -Destination (Join-Path $Runtime $name) -Force
    }
}

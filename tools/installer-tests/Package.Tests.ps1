#Requires -Version 5.1
param(
    [Parameter(Mandatory = $true)][string] $RepoRoot,
    [Parameter(Mandatory = $true)][string] $Root,
    [Parameter(Mandatory = $true)][string] $Zip,
    [string] $ReShade64
)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

$validator = Join-Path $RepoRoot 'tools\Test-ReleasePackage.ps1'
$provided = [IO.Path]::GetFullPath($Zip)
$testsRoot = [IO.Path]::GetFullPath((Join-Path $Root 'package-negative'))
$rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\')
if (-not $testsRoot.StartsWith($rootFull + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Package test output escapes its temporary root.'
}
New-Item -ItemType Directory -Path $testsRoot -Force | Out-Null

function Invoke-ZipValidator([string] $Path) {
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $validator -Zip $Path 2>&1 | Out-String
    $code = $LASTEXITCODE
    $ErrorActionPreference = $oldPreference
    return [pscustomobject]@{ Code = $code; Output = $out }
}

function New-MutatedZip([string] $Name, [scriptblock] $Mutation) {
    $dir = Join-Path $testsRoot $Name
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    [IO.Compression.ZipFile]::ExtractToDirectory($original, $dir)
    & $Mutation $dir
    $output = Join-Path $testsRoot ($Name + '.zip')
    $archive = [IO.Compression.ZipFile]::Open($output, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in @(Get-ChildItem -LiteralPath $dir -File -Recurse | Sort-Object FullName)) {
            $relative = $file.FullName.Substring($dir.Length).TrimStart([char]92, [char]47).Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $archive, $file.FullName, $relative, [IO.Compression.CompressionLevel]::Optimal)
        }
    }
    finally { $archive.Dispose() }
    return $output
}

$providedResult = Invoke-ZipValidator $provided
Check 'gate release zip validates' ($providedResult.Code -eq 0) $providedResult.Output
$packageOut = Join-Path $Root 'package-built'
$oldPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$packageOutput = & powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File (Join-Path $RepoRoot 'tools\Package-Release.ps1') `
    -BuildDirX64 (Join-Path $RepoRoot 'out\build\x64-mt') `
    -BuildDirX86 (Join-Path $RepoRoot 'out\build\x86-remote') -Out $packageOut 2>&1 | Out-String
$packageCode = $LASTEXITCODE
$ErrorActionPreference = $oldPreference
Check 'package builds inside temporary test root' ($packageCode -eq 0) $packageOutput
if ($packageCode -ne 0) { throw 'Temporary package build failed' }
$original = Join-Path $packageOut 'Optimizer-FPS-for-DLSS5-2026.9.1.zip'
$extract = Join-Path $Root 'package-extracted'
New-Item -ItemType Directory -Path $extract -Force | Out-Null
[IO.Compression.ZipFile]::ExtractToDirectory($original, $extract)
$good = Invoke-ZipValidator $original
Check 'clean release zip validates' ($good.Code -eq 0) $good.Output
$archive = [IO.Compression.ZipFile]::OpenRead($original)
try {
    $paths = @($archive.Entries | ForEach-Object { $_.FullName })
    $modules = @($paths | Where-Object { $_ -match '/installer/[^/]+\.psm1$' })
    $sourceModules = @(Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'tools\installer') -File -Filter '*.psm1')
    $expectedModules = @($sourceModules | ForEach-Object { 'installer/' + $_.Name } | Sort-Object)
    $actualModules = @($modules | ForEach-Object {
        $_.Substring($_.IndexOf('/installer/') + 1)
    } | Sort-Object)
    Check 'release zip contains every installer module' `
        (@(Compare-Object $expectedModules $actualModules).Count -eq 0)
    Check 'zip uses slash paths and no traversal' `
        (@($paths | Where-Object { $_ -match '\\|(^/|^[A-Za-z]:|(^|/)\.\.(/|$))' }).Count -eq 0)
}
finally { $archive.Dispose() }

$stage = Join-Path $extract ([IO.Path]::GetFileNameWithoutExtension($original))
$payload = Join-Path $stage 'payload'
$buildX64 = Join-Path $RepoRoot 'out\build\x64-mt'
$buildX86 = Join-Path $RepoRoot 'out\build\x86-remote'
$sources = @{
    'x64\optimizer-fps-dlss5.addon64' = Join-Path $buildX64 'hosts\reshade\Release\optimizer-fps-dlss5.addon64'
    'x64\optimizer-fps-dlss5-core.dll' = Join-Path $buildX64 'core\Release\optimizer-fps-dlss5-core.dll'
    'x64\nvngx.dll_optimizerfps.dll' = Join-Path $buildX64 'hosts\reshade\ngx_forwarder\Release\nvngx.dll_optimizerfps.dll'
    'x86\optimizer-fps-dlss5-remote.addon32' = Join-Path $buildX86 'hosts\remote32\Release\optimizer-fps-dlss5-remote.addon32'
}
foreach ($shader in @(Get-ChildItem -LiteralPath (Join-Path $buildX64 'shaders') -File -Filter '*.dxbc')) {
    $sources[('x64\optimizer-fps-dlss5\' + $shader.Name)] = $shader.FullName
}
$matchesBuild = $true
foreach ($relative in $sources.Keys) {
    $staged = Join-Path $payload $relative
    if (-not (Test-Path -LiteralPath $staged -PathType Leaf) -or
        (Get-FileHash -LiteralPath $staged).Hash -ne (Get-FileHash -LiteralPath $sources[$relative]).Hash) {
        $matchesBuild = $false
        break
    }
}
$payloadFiles = @(Get-ChildItem -LiteralPath $payload -File -Recurse)
Check 'staged payload exactly matches current build files' `
    ($matchesBuild -and $payloadFiles.Count -eq $sources.Count + 2)
$modulesMatch = $true
foreach ($module in $sourceModules) {
    $staged = Join-Path $stage ('installer\' + $module.Name)
    if (-not (Test-Path -LiteralPath $staged -PathType Leaf) -or
        (Get-FileHash -LiteralPath $staged).Hash -ne (Get-FileHash -LiteralPath $module.FullName).Hash) {
        $modulesMatch = $false
        break
    }
}
Check 'staged installer modules match source' $modulesMatch

if ($ReShade64 -and (Test-Path -LiteralPath $ReShade64 -PathType Leaf)) {
    $game = New-X64Fixture -Name 'package-standalone' -Root $Root -ReShade64 $ReShade64
    $entry = Join-Path $stage 'Install-OptimizerFPS.ps1'
    $verify = Join-Path $stage 'Verify-OptimizerFPS.ps1'
    $standalone = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $entry `
        -GameExe (Join-Path $game 'pwgame.exe') -Payload $payload -Yes -NoPause -NoVerify 2>&1 | Out-String
    $code = $LASTEXITCODE
    Check 'extracted package installs without repository scripts' `
        ($code -eq 0 -or $code -eq 10) $standalone
    $verified = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $verify `
        -GameExe (Join-Path $game 'pwgame.exe') -Payload $payload -Json 2>&1 | Out-String
    $code = $LASTEXITCODE
    $json = $null
    try { $json = $verified | ConvertFrom-Json } catch { }
    Check 'extracted package Verify checks installed core' `
        ($code -eq 10 -and $json -and $json.Static.Core.StaticVerdict -eq 'ok') $verified

    $cmdGame = New-X64Fixture -Name 'package-cmd-launcher' -Root $Root -ReShade64 $ReShade64
    $cmdEntry = Join-Path $stage 'Install-OptimizerFPS.cmd'
    $cmdOutput = & $cmdEntry (Join-Path $cmdGame 'pwgame.exe') -Yes -NoPause -NoVerify 2>&1 | Out-String
    $cmdCode = $LASTEXITCODE
    Check 'extracted CMD launcher honors -NoPause' `
        ($cmdCode -eq 0 -and (Test-Path -LiteralPath (Join-Path $cmdGame 'optimizer-fps-dlss5.addon64'))) $cmdOutput

    $missingTree = Join-Path $Root 'package-missing-module-tree'
    Copy-Item -LiteralPath $stage -Destination $missingTree -Recurse
    Remove-Item -LiteralPath (Join-Path $missingTree 'installer\IniMigration.psm1') -Force
    $missingGame = New-X64Fixture -Name 'package-missing-module-game' `
        -Root $Root -ReShade64 $ReShade64
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $missingResult = & powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $missingTree 'Install-OptimizerFPS.ps1') `
        -GameExe (Join-Path $missingGame 'pwgame.exe') `
        -Payload (Join-Path $missingTree 'payload') -Yes -NoPause -NoVerify 2>&1 | Out-String
    $missingCode = $LASTEXITCODE
    $ErrorActionPreference = $oldPreference
    Check 'missing module prevents any game write' `
        ($missingCode -ne 0 -and
         -not (Test-Path -LiteralPath (Join-Path $missingGame 'optimizer-fps-dlss5.addon64'))) $missingResult
}
else { Skip 'extracted package Install and Verify' 'ReShade64 fixture absent' }

$withoutCore = New-MutatedZip 'missing-core' {
    param($dir)
    $file = @(Get-ChildItem -LiteralPath $dir -File -Recurse -Filter 'optimizer-fps-dlss5-core.dll')[0]
    Remove-Item -LiteralPath $file.FullName -Force
}
$bad = Invoke-ZipValidator $withoutCore
Check 'missing core is rejected' ($bad.Code -ne 0) $bad.Output

$withoutShader = New-MutatedZip 'missing-shader' {
    param($dir)
    $file = @(Get-ChildItem -LiteralPath $dir -File -Recurse -Filter '*.dxbc')[0]
    Remove-Item -LiteralPath $file.FullName -Force
}
$bad = Invoke-ZipValidator $withoutShader
Check 'missing shader is rejected' ($bad.Code -ne 0) $bad.Output

$changedShader = New-MutatedZip 'changed-shader' {
    param($dir)
    $file = @(Get-ChildItem -LiteralPath $dir -File -Recurse -Filter '*.dxbc')[0]
    [IO.File]::WriteAllBytes($file.FullName, [byte[]]@(1,2,3,4))
}
$bad = Invoke-ZipValidator $changedShader
Check 'shader hash mismatch is rejected' ($bad.Code -ne 0) $bad.Output

$extraShader = New-MutatedZip 'extra-shader' {
    param($dir)
    $folder = @(Get-ChildItem -LiteralPath $dir -Directory -Recurse |
        Where-Object { $_.Name -eq 'optimizer-fps-dlss5' })[0]
    [IO.File]::WriteAllBytes((Join-Path $folder.FullName 'stale.dxbc'), [byte[]]@(1,2,3,4))
}
$bad = Invoke-ZipValidator $extraShader
Check 'unlisted shader is rejected' ($bad.Code -ne 0) $bad.Output

$withoutModule = New-MutatedZip 'missing-module' {
    param($dir)
    $file = @(Get-ChildItem -LiteralPath $dir -File -Recurse -Filter 'InstallFiles.psm1')[0]
    Remove-Item -LiteralPath $file.FullName -Force
}
$bad = Invoke-ZipValidator $withoutModule
Check 'missing installer module is rejected' ($bad.Code -ne 0) $bad.Output

$duplicate = Join-Path $testsRoot 'duplicate-entry.zip'
Copy-Item -LiteralPath $original -Destination $duplicate
$archive = [IO.Compression.ZipFile]::Open($duplicate, [IO.Compression.ZipArchiveMode]::Update)
try {
    $entry = @($archive.Entries | Where-Object { $_.FullName -match '/payload/VERSION\.txt$' })[0]
    $copy = $archive.CreateEntry($entry.FullName)
    $stream = $copy.Open()
    try { $bytes = [Text.Encoding]::ASCII.GetBytes('duplicate'); $stream.Write($bytes, 0, $bytes.Length) }
    finally { $stream.Dispose() }
}
finally { $archive.Dispose() }
$bad = Invoke-ZipValidator $duplicate
Check 'duplicate zip entry is rejected' ($bad.Code -ne 0) $bad.Output

foreach ($unsafe in @('../outside.txt','/absolute.txt','C:/drive.txt')) {
    $unsafeZip = Join-Path $testsRoot (('unsafe-' + [Guid]::NewGuid().ToString('N')) + '.zip')
    Copy-Item -LiteralPath $original -Destination $unsafeZip
    $archive = [IO.Compression.ZipFile]::Open($unsafeZip, [IO.Compression.ZipArchiveMode]::Update)
    try {
        $entry = $archive.CreateEntry($unsafe)
        $stream = $entry.Open()
        try { $stream.WriteByte(1) } finally { $stream.Dispose() }
    }
    finally { $archive.Dispose() }
    $bad = Invoke-ZipValidator $unsafeZip
    Check ('unsafe zip path rejected: ' + $unsafe) ($bad.Code -ne 0) $bad.Output
}

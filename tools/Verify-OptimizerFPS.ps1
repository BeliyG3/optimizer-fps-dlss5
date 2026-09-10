#Requires -Version 5.1
<#
.SYNOPSIS
    Checks an "Optimizer FPS for DLSS5" install: the files on disk, and what ReShade.log
    says the add-on actually did the last time the game ran.

.DESCRIPTION
    Copyright (c) 2026 Yuri Grib (BeliyG3). MIT licence (see LICENSE).

    Single file on purpose (it ships beside the installer and has to run from an unpacked
    zip with nothing else present), so the few PE/ini helpers it needs are duplicated from
    Install-OptimizerFPS.ps1 rather than dot-sourced. Some helpers were adapted from the
    DLSS5-Feeder installer (MIT).

    Exit codes: 0 everything checks out, 10 installed but the game has not run with it yet,
    1 problems found.

.EXAMPLE
    .\Verify-OptimizerFPS.ps1 "D:\Games\Baldurs Gate 3\bin\bg3_dx11.exe"
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string] $GameExe,

    [string] $Payload,
    [switch] $Json,
    [switch] $Quiet
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$script:ExitOk          = 0
$script:ExitProblems    = 1
$script:ExitNotVerified = 10

$script:ReShadeMinVersion = '6.8'
$script:ReShadeDllNames   = @('dxgi.dll', 'd3d11.dll', 'd3d12.dll', 'd3d9.dll', 'opengl32.dll', 'ReShade64.dll', 'ReShade32.dll')
$script:AddonName         = 'Optimizer FPS for DLSS5'
$script:RemoteAddonName   = 'Optimizer FPS for DLSS5 (tab for the 64-bit host)'
$script:AddonFileName     = 'optimizer-fps-dlss5.addon64'
$script:ForwarderFileName = 'nvngx.dll_optimizerfps.dll'
$script:RemoteFileName    = 'optimizer-fps-dlss5-remote.addon32'
$script:ShaderFolderName  = 'optimizer-fps-dlss5'
$script:SessionMarker     = 'optimizer-fps-dlss5.session'

# What the same files were called before 26.26. Still lying around, they are only clutter --
# except the .addon64, which ReShade would load a second time.
$script:LegacyAddonFileName     = 'peripheral-warp.addon64'
$script:LegacyForwarderFileName = 'nvngx.dll_peripheralwarp.dll'
$script:LegacyRemoteFileName    = 'peripheral-warp-remote.addon32'
$script:LegacyShaderFolderName  = 'peripheral-warp'
$script:LegacySessionMarker     = 'peripheral-warp.session'
$script:StateFolderName   = '_OptimizerFPS'
$script:HostExeName       = 'dlss5-feed-host64.exe'

$script:CountWarn = 0
$script:CountFail = 0
$script:WarnLines = New-Object System.Collections.ArrayList
$script:FailLines = New-Object System.Collections.ArrayList

$script:Result = [ordered]@{
    Tool      = 'Verify-OptimizerFPS'
    ExitCode  = 1
    Arch      = $null
    GameExe   = $null
    TargetDir = $null
    AddonDir  = $null
    RemoteDir = $null
    Version   = $null
    Static    = [ordered]@{}
    Runtime   = [ordered]@{}
    Warnings  = @()
    Failures  = @()
}

$script:UseColour = $true
try {
    if ($null -eq $Host -or $null -eq $Host.UI -or $null -eq $Host.UI.RawUI) { $script:UseColour = $false }
    else { $null = $Host.UI.RawUI.ForegroundColor }
}
catch { $script:UseColour = $false }

function Write-Chunk
{
    param([string] $Text, [string] $Colour, [switch] $NoNewline)
    if ($Json -or $Quiet) { return }
    try {
        if ($script:UseColour -and $Colour) { Write-Host $Text -ForegroundColor $Colour -NoNewline:$NoNewline }
        else { Write-Host $Text -NoNewline:$NoNewline }
    }
    catch { $script:UseColour = $false; Write-Host $Text -NoNewline:$NoNewline }
}

function Write-Section
{
    param([string] $Title)
    if ($Json -or $Quiet) { return }
    Write-Host ''
    Write-Chunk ('  ' + [char]0x2500 + [char]0x2500 + ' ') 'Cyan' -NoNewline
    Write-Chunk $Title 'White' -NoNewline
    $pad = 62 - $Title.Length
    if ($pad -lt 1) { $pad = 1 }
    Write-Chunk (' ' + ([string][char]0x2500) * $pad) 'Cyan'
}

function Report
{
    param(
        [ValidateSet('Ok', 'Skip', 'Warn', 'Fail', 'Info')]
        [string] $Status,
        [string] $Text,
        [string] $Detail
    )
    $glyph = '[ .. ]'; $colour = 'DarkGray'
    switch ($Status) {
        'Ok'   { $glyph = '[ OK ]'; $colour = 'Green' }
        'Skip' { $glyph = '[ -- ]'; $colour = 'DarkGray' }
        'Warn' { $glyph = '[WARN]'; $colour = 'Yellow'; $script:CountWarn++; $null = $script:WarnLines.Add($Text) }
        'Fail' { $glyph = '[FAIL]'; $colour = 'Red';    $script:CountFail++; $null = $script:FailLines.Add($Text) }
        'Info' { $glyph = '[ .. ]'; $colour = 'DarkGray' }
    }
    if ($Json -or $Quiet) { return }
    Write-Chunk ('  ' + $glyph + ' ') $colour -NoNewline
    if ($Status -eq 'Skip' -or $Status -eq 'Info') { Write-Chunk $Text 'DarkGray' } else { Write-Host $Text }
    if ($Detail) {
        foreach ($line in ($Detail -split "`n")) { if ($line.Trim()) { Write-Chunk ('         ' + $line.Trim()) 'DarkGray' } }
    }
}

function Exit-Verify
{
    param([int] $Code)
    if ($Json) {
        $script:Result.ExitCode = $Code
        $script:Result.Warnings = @($script:WarnLines.ToArray())
        $script:Result.Failures = @($script:FailLines.ToArray())
        Write-Output ($script:Result | ConvertTo-Json -Depth 12)
    }
    exit $Code
}

# ---------------------------------------------------------------------------------------
# Helpers (duplicated from Install-OptimizerFPS.ps1 -- single-file rule)
# ---------------------------------------------------------------------------------------

function Join-Safe { param([string] $Parent, [string] $Child) try { return [IO.Path]::Combine($Parent, $Child) } catch { return $null } }
function Test-FileHere { param([string] $Path) if (-not $Path) { return $false } try { return (Test-Path -LiteralPath $Path -PathType Leaf) } catch { return $false } }
function Test-DirHere { param([string] $Path) if (-not $Path) { return $false } try { return (Test-Path -LiteralPath $Path -PathType Container) } catch { return $false } }
function Get-Sha256 { param([string] $Path) try { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash.ToUpperInvariant() } catch { return $null } }

function Get-FileVersionSafe
{
    param([string] $Path)
    if (-not (Test-FileHere $Path)) { return $null }
    try {
        $vi = (Get-Item -LiteralPath $Path -ErrorAction Stop).VersionInfo
        if ($vi -and $vi.FileVersion) { return ($vi.FileVersion.Trim() -replace '\s*,\s*', '.') }
    }
    catch { }
    return $null
}

function Read-TextSafe
{
    param([string] $Path)
    if (-not (Test-FileHere $Path)) { return $null }
    try {
        $t = [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8)
        if ($t.Length -gt 0 -and [int]$t[0] -eq 0xFEFF) { $t = $t.Substring(1) }
        return $t
    }
    catch { return $null }
}

function Get-RelativePathCompat
{
    param([string] $Base, [string] $Path)
    try {
        $b = [IO.Path]::GetFullPath($Base)
        if (-not $b.EndsWith('\')) { $b += '\' }
        $p = [IO.Path]::GetFullPath($Path)
        if ($p.StartsWith($b, [StringComparison]::OrdinalIgnoreCase)) { return $p.Substring($b.Length) }
        return $p
    }
    catch { return $Path }
}

function Convert-RvaToOffset
{
    param($Sections, [uint32] $Rva, [long] $Length)
    foreach ($s in $Sections) {
        if ($Rva -ge $s.V -and $Rva -lt ($s.V + $s.Span)) {
            $o = [long]$s.Raw + ([long]$Rva - [long]$s.V)
            if ($o -ge 0 -and $o -lt $Length) { return [long]$o }
            return [long](-1)
        }
    }
    return [long](-1)
}

function Get-PeBits
{
    param([string] $Path)
    if (-not (Test-FileHere $Path)) { return 0 }
    $fs = $null; $br = $null
    try {
        $fs = New-Object IO.FileStream($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        if ($fs.Length -lt 0x40) { return 0 }
        $br = New-Object IO.BinaryReader($fs)
        if ($br.ReadUInt16() -ne 0x5A4D) { return 0 }
        $fs.Position = 0x3C
        $peOff = $br.ReadInt32()
        if ($peOff -le 0 -or ($peOff + 24) -ge $fs.Length) { return 0 }
        $fs.Position = $peOff
        if ($br.ReadUInt32() -ne 0x00004550) { return 0 }
        $machine = $br.ReadUInt16()
        if ($machine -eq 0x014C -or $machine -eq 0x01C4) { return 32 }
        if ($machine -eq 0x8664 -or $machine -eq 0xAA64) { return 64 }
        return 0
    }
    catch { return 0 }
    finally {
        if ($br) { try { $br.Close() } catch { } }
        if ($fs) { try { $fs.Dispose() } catch { } }
    }
}

function Get-PeExportNames
{
    param([string] $Path, [int] $Max = 8192)
    if (-not (Test-FileHere $Path)) { return $null }
    $fs = $null; $br = $null
    try {
        $fs = New-Object IO.FileStream($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        if ($fs.Length -lt 0x40) { return $null }
        $br = New-Object IO.BinaryReader($fs)
        if ($br.ReadUInt16() -ne 0x5A4D) { return $null }
        $fs.Position = 0x3C
        $peOff = $br.ReadInt32()
        if ($peOff -le 0 -or ($peOff + 24) -ge $fs.Length) { return $null }
        $fs.Position = $peOff
        if ($br.ReadUInt32() -ne 0x00004550) { return $null }
        $null      = $br.ReadUInt16()
        $nSections = $br.ReadUInt16()
        $null      = $br.ReadUInt32()
        $null      = $br.ReadUInt32()
        $null      = $br.ReadUInt32()
        $optSize   = $br.ReadUInt16()
        $null      = $br.ReadUInt16()

        $optOff = [long]$peOff + 24
        if ($optSize -lt 96 -or ($optOff + $optSize) -gt $fs.Length) { return $null }
        $fs.Position = $optOff
        $magic = $br.ReadUInt16()
        $dirOff = 0
        if     ($magic -eq 0x20B) { $dirOff = $optOff + 112 }
        elseif ($magic -eq 0x10B) { $dirOff = $optOff + 96 }
        else                      { return $null }
        if (($dirOff + 8) -gt $fs.Length) { return $null }
        $fs.Position = $dirOff
        $expRva = $br.ReadUInt32()
        $null   = $br.ReadUInt32()
        if ($expRva -eq 0) { return ,([string[]] @()) }

        $secOff = $optOff + $optSize
        $sections = @()
        for ($i = 0; $i -lt $nSections; $i++) {
            $p = $secOff + ([long]$i * 40)
            if (($p + 40) -gt $fs.Length) { break }
            $fs.Position = $p + 8
            $vsize = $br.ReadUInt32(); $vaddr = $br.ReadUInt32(); $rsize = $br.ReadUInt32(); $raw = $br.ReadUInt32()
            $span = $rsize
            if ($vsize -gt 0) { $span = $vsize }
            $sections += New-Object psobject -Property @{ V = $vaddr; Span = $span; Raw = $raw }
        }
        if ($sections.Count -eq 0) { return $null }

        $eo = Convert-RvaToOffset $sections $expRva $fs.Length
        if ($eo -lt 0 -or ($eo + 40) -gt $fs.Length) { return $null }
        $fs.Position = $eo + 20
        $null     = $br.ReadUInt32()
        $nNames   = $br.ReadUInt32()
        $null     = $br.ReadUInt32()
        $namesRva = $br.ReadUInt32()
        if ($nNames -eq 0 -or $namesRva -eq 0) { return ,([string[]] @()) }
        if ($nNames -gt $Max) { $nNames = $Max }
        $no = Convert-RvaToOffset $sections $namesRva $fs.Length
        if ($no -lt 0) { return $null }

        $names = New-Object 'System.Collections.Generic.List[string]'
        for ($i = 0; $i -lt $nNames; $i++) {
            $p = $no + ([long]$i * 4)
            if (($p + 4) -gt $fs.Length) { break }
            $fs.Position = $p
            $so = Convert-RvaToOffset $sections ($br.ReadUInt32()) $fs.Length
            if ($so -lt 0) { continue }
            $fs.Position = $so
            $sb = New-Object Text.StringBuilder
            for ($k = 0; $k -lt 256; $k++) {
                $b = $fs.ReadByte()
                if ($b -le 0) { break }
                $null = $sb.Append([char]$b)
            }
            if ($sb.Length -gt 0) { $null = $names.Add($sb.ToString()) }
        }
        return ,([string[]] $names.ToArray())
    }
    catch { return $null }
    finally {
        if ($br) { try { $br.Close() } catch { } }
        if ($fs) { try { $fs.Dispose() } catch { } }
    }
}

function Test-ReShadeHasAddons
{
    param([string] $Path)
    $names = Get-PeExportNames $Path
    if ($null -eq $names) { return $null }
    return [bool](($names -contains 'ReShadeRegisterAddon') -and ($names -contains 'ReShadeUnregisterAddon'))
}

function Get-IniKey
{
    param([string] $Text, [string] $Section, [string] $Key)
    if ($null -eq $Text) { return $null }
    $cur = ''
    foreach ($l in ($Text -split "`r?`n")) {
        $t = $l.Trim()
        if ($t -match '^\[(.+)\]$') { $cur = $Matches[1]; continue }
        if ($cur -ieq $Section -and $t -match ('(?i)^' + [regex]::Escape($Key) + '\s*=\s*(.*)$')) { return $Matches[1] }
    }
    return $null
}

function Test-IniSection
{
    param([string] $Text, [string] $Section)
    if ($null -eq $Text) { return $false }
    foreach ($l in ($Text -split "`r?`n")) {
        $t = $l.Trim()
        if ($t -match '^\[(.+)\]$' -and $Matches[1] -ieq $Section) { return $true }
    }
    return $false
}

function Find-ReShadeDll
{
    param([string] $Dir, [int] $Bits)
    $best = $null
    foreach ($n in $script:ReShadeDllNames) {
        $p = Join-Safe $Dir $n
        if (-not (Test-FileHere $p)) { continue }
        if ((Get-PeBits $p) -ne $Bits) { continue }
        if ((Test-ReShadeHasAddons $p) -eq $true) { return $p }
        if ($null -eq $best) {
            $pn = $null
            try { $pn = (Get-Item -LiteralPath $p).VersionInfo.ProductName } catch { }
            if ($pn -and $pn -match '(?i)reshade') { $best = $p }
        }
    }
    return $best
}

# ---------------------------------------------------------------------------------------
# Resolve the game
# ---------------------------------------------------------------------------------------

if (-not $GameExe) {
    Report -Status 'Fail' -Text 'No game given.' -Detail 'Usage: Verify-OptimizerFPS.ps1 <game .exe or game folder> [-Json]'
    Exit-Verify $script:ExitProblems
}

$gamePath = $null
try { $gamePath = (Resolve-Path -LiteralPath $GameExe -ErrorAction Stop).ProviderPath } catch { }
if (-not $gamePath) {
    Report -Status 'Fail' -Text ('Path not found: ' + $GameExe)
    Exit-Verify $script:ExitProblems
}

$gameExePath = $null
$gameRoot    = $null
if (Test-DirHere $gamePath) {
    $gameRoot = $gamePath
    $exes = @(Get-ChildItem -LiteralPath $gameRoot -File -Filter '*.exe' -ErrorAction SilentlyContinue |
              Where-Object { $_.Name -ine $script:HostExeName })
    if ($exes.Count -ne 1) {
        Report -Status 'Fail' -Text 'Point this at the game''s .exe: the folder holds several.' -Detail (($exes | ForEach-Object { $_.Name }) -join "`n")
        Exit-Verify $script:ExitProblems
    }
    $gameExePath = $exes[0].FullName
}
else {
    $gameExePath = $gamePath
    $gameRoot    = Split-Path -Parent $gamePath
}

$bits = Get-PeBits $gameExePath
$arch = 'x64'
if ($bits -eq 32) { $arch = 'x86' }
elseif ($bits -ne 64) {
    Report -Status 'Fail' -Text ('Not a readable 32/64-bit executable: ' + $gameExePath)
    Exit-Verify $script:ExitProblems
}

$script:Result.Arch    = $arch
$script:Result.GameExe = $gameExePath

$targetDir = $null
$remoteDir = $null
$reshadeDll = $null
if ($arch -eq 'x64') {
    $reshadeDll = Find-ReShadeDll -Dir $gameRoot -Bits 64
    if ($reshadeDll) { $targetDir = Split-Path -Parent $reshadeDll }
}
else {
    $host64 = Join-Safe $gameRoot 'host64'
    $reshadeDll = Find-ReShadeDll -Dir $host64 -Bits 64
    if ($reshadeDll) { $targetDir = $host64 }
    $r32 = Find-ReShadeDll -Dir $gameRoot -Bits 32
    if (-not $r32) {
        $binDir = Join-Safe $gameRoot 'bin'
        if (Test-DirHere $binDir) { $r32 = Find-ReShadeDll -Dir $binDir -Bits 32 }
    }
    if ($r32) { $remoteDir = Split-Path -Parent $r32 }
}

if (-not $targetDir) {
    Report -Status 'Fail' -Text 'No ReShade DLL found for this game; nothing is installed.'
    Exit-Verify $script:ExitProblems
}

$iniPath = Join-Safe $targetDir 'ReShade.ini'
$iniText = Read-TextSafe $iniPath
$addonDir = $targetDir
$addonPathKey = Get-IniKey $iniText 'ADDON' 'AddonPath'
if ($addonPathKey) {
    $first = ($addonPathKey -split ',')[0].Trim()
    if ($first) {
        $resolved = $first
        if (-not [IO.Path]::IsPathRooted($resolved)) { $resolved = Join-Safe $targetDir $resolved }
        try { $resolved = [IO.Path]::GetFullPath($resolved) } catch { }
        if (Test-DirHere $resolved) { $addonDir = $resolved }
    }
}

$script:Result.TargetDir = $targetDir
$script:Result.AddonDir  = $addonDir
$script:Result.RemoteDir = $remoteDir

Write-Section ('Optimizer FPS for DLSS5 -- ' + [IO.Path]::GetFileName($gameExePath) + ' (' + $arch + ')')
Report -Status 'Info' -Text ('ReShade: ' + $targetDir)

# ---------------------------------------------------------------------------------------
# Static checks
# ---------------------------------------------------------------------------------------

Write-Section 'Files'

$stateDir   = Join-Safe $targetDir $script:StateFolderName
$latestPath = Join-Safe $stateDir 'latest-receipt.json'
$receipt = $null
if (Test-FileHere $latestPath) {
    try { $receipt = (Read-TextSafe $latestPath) | ConvertFrom-Json } catch { $receipt = $null }
}
if ($receipt -and $receipt.PSObject.Properties['Version']) { $script:Result.Version = [string]$receipt.Version }

if (-not $Payload) { $Payload = Join-Path $PSScriptRoot 'payload' }
$manifest = @{}
$manifestSource = $null
$manifestPath = Join-Safe $Payload 'files.sha256'
if (Test-FileHere $manifestPath) {
    foreach ($line in ((Read-TextSafe $manifestPath) -split "`r?`n")) {
        $t = $line.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        $m = [regex]::Match($t, '^([0-9a-fA-F]{64})\s+(.+)$')
        if ($m.Success) { $manifest[$m.Groups[2].Value.Trim().Replace('\', '/')] = $m.Groups[1].Value.ToUpperInvariant() }
    }
    $manifestSource = 'payload'
    if (-not $script:Result.Version) {
        $vf = Join-Safe $Payload 'VERSION.txt'
        if (Test-FileHere $vf) { $script:Result.Version = (Read-TextSafe $vf).Trim() }
    }
}
elseif ($receipt) { $manifestSource = 'receipt' }

# Expected destinations, whether or not there is a payload beside this script.
$expected = New-Object System.Collections.ArrayList
$null = $expected.Add(@{ Path = (Join-Safe $addonDir $script:AddonFileName);     Rel = ('x64/' + $script:AddonFileName) })
$null = $expected.Add(@{ Path = (Join-Safe $addonDir $script:ForwarderFileName); Rel = ('x64/' + $script:ForwarderFileName) })
if ($arch -eq 'x86' -and $remoteDir) {
    $null = $expected.Add(@{ Path = (Join-Safe $remoteDir $script:RemoteFileName); Rel = ('x86/' + $script:RemoteFileName) })
}

$receiptHashes = @{}
if ($receipt -and $receipt.PSObject.Properties['Files']) {
    foreach ($f in @($receipt.Files)) { $receiptHashes[([string]$f.Path).ToLowerInvariant()] = [string]$f.InstalledHash }
}

$missing = 0
$mismatched = 0
foreach ($e in $expected) {
    if (-not (Test-FileHere $e.Path)) {
        Report -Status 'Fail' -Text ('Missing: ' + (Get-RelativePathCompat $gameRoot $e.Path))
        $missing++
        continue
    }
    $now = Get-Sha256 $e.Path
    $want = $null
    if ($manifestSource -eq 'payload' -and $manifest.ContainsKey($e.Rel)) { $want = $manifest[$e.Rel] }
    elseif ($receiptHashes.ContainsKey($e.Path.ToLowerInvariant())) { $want = $receiptHashes[$e.Path.ToLowerInvariant()] }
    if ($want -and -not [string]::Equals($now, $want, [StringComparison]::OrdinalIgnoreCase)) {
        Report -Status 'Warn' -Text ('Different from the reference: ' + [IO.Path]::GetFileName($e.Path))
        $mismatched++
    }
    else {
        Report -Status 'Ok' -Text ([IO.Path]::GetFileName($e.Path) + ' present')
    }
}

$script:Result.Static['MissingFiles'] = $missing
$script:Result.Static['ChangedFiles'] = $mismatched
$script:Result.Static['HashSource']   = $manifestSource

# The forwarder must export the three entry points the add-on calls.
$fwd = Join-Safe $addonDir $script:ForwarderFileName
if (Test-FileHere $fwd) {
    $names = Get-PeExportNames $fwd
    $ok = $false
    if ($null -ne $names) {
        $ok = ($names -contains 'pw_ngx_call_create') -and ($names -contains 'pw_ngx_call_evaluate') -and ($names -contains 'pw_ngx_call_release')
    }
    $script:Result.Static['ForwarderExports'] = $ok
    if ($ok) { Report -Status 'Ok' -Text 'The NGX forwarder exports pw_ngx_call_create/evaluate/release.' }
    else { Report -Status 'Fail' -Text 'nvngx.dll_optimizerfps.dll does not export the entry points the add-on calls.' }
}

# The add-on itself.
$addonPath = Join-Safe $addonDir $script:AddonFileName
if (Test-FileHere $addonPath) {
    $names = Get-PeExportNames $addonPath
    $isOurs = $false
    if ($null -ne $names) { $isOurs = ($names -contains 'PeripheralWarpSetLayoutV1') -or ($names -contains 'PeripheralWarpSetTemporalV1') }
    $script:Result.Static['AddonIsOurs'] = $isOurs
    if (-not $isOurs) { Report -Status 'Fail' -Text 'optimizer-fps-dlss5.addon64 is not an Optimizer FPS build.' }
}

# Shaders.
$shaderDir = Join-Safe $addonDir $script:ShaderFolderName
$shaderCount = 0
if (Test-DirHere $shaderDir) { $shaderCount = @(Get-ChildItem -LiteralPath $shaderDir -File -Filter '*.dxbc' -ErrorAction SilentlyContinue).Count }
$script:Result.Static['ShaderCount'] = $shaderCount
$wantShaders = @($manifest.Keys | Where-Object { $_ -match '(?i)^x64/optimizer-fps-dlss5/.+\.dxbc$' }).Count
if ($shaderCount -eq 0) {
    Report -Status 'Fail' -Text ('No compiled shaders in ' + $shaderDir + '; the add-on will forward every frame untouched.')
}
elseif ($wantShaders -gt 0 -and $shaderCount -ne $wantShaders) {
    Report -Status 'Warn' -Text ('' + $shaderCount + ' shader(s) present, the payload has ' + $wantShaders + '.')
}
else {
    Report -Status 'Ok' -Text ('' + $shaderCount + ' compiled shader(s) in optimizer-fps-dlss5\.')
}

# Files under the names this add-on used before 26.26.
$legacyPresent = New-Object System.Collections.ArrayList
foreach ($n in @($script:LegacyAddonFileName, $script:LegacyForwarderFileName, $script:LegacySessionMarker)) {
    $p = Join-Safe $addonDir $n
    if (Test-FileHere $p) { $null = $legacyPresent.Add($p) }
}
$legacyShaderDir = Join-Safe $addonDir $script:LegacyShaderFolderName
if (Test-DirHere $legacyShaderDir) { $null = $legacyPresent.Add($legacyShaderDir) }
if ($remoteDir) {
    $p = Join-Safe $remoteDir $script:LegacyRemoteFileName
    if (Test-FileHere $p) { $null = $legacyPresent.Add($p) }
}
$script:Result.Static['LegacyFiles'] = @($legacyPresent.ToArray())
if ($legacyPresent.Count -gt 0) {
    Report -Status 'Warn' -Text ('' + $legacyPresent.Count + ' legacy file(s) from an earlier release are still present; run Update.') `
           -Detail (($legacyPresent | ForEach-Object { [IO.Path]::GetFileName($_) }) -join "`n")
}

# ReShade build.
$reshadeVersion = Get-FileVersionSafe $reshadeDll
$hasAddons = Test-ReShadeHasAddons $reshadeDll
$script:Result.Static['ReShadeVersion'] = $reshadeVersion
$script:Result.Static['ReShadeAddons']  = $hasAddons
if ($hasAddons -eq $false) { Report -Status 'Fail' -Text 'That ReShade build has no add-on support.' }
else {
    $vm = [regex]::Match([string]$reshadeVersion, '^\s*(\d+)\.(\d+)')
    $want = $script:ReShadeMinVersion -split '\.'
    if ($vm.Success -and ([int]$vm.Groups[1].Value -lt [int]$want[0] -or ([int]$vm.Groups[1].Value -eq [int]$want[0] -and [int]$vm.Groups[2].Value -lt [int]$want[1]))) {
        Report -Status 'Fail' -Text ('ReShade ' + $reshadeVersion + ' is too old (need ' + $script:ReShadeMinVersion + '+).')
    }
    else {
        $vt = $reshadeVersion
        if (-not $vt) { $vt = 'unknown version' }
        Report -Status 'Ok' -Text ('ReShade ' + $vt + ', add-on build.')
    }
}

Write-Section 'Neural rendering'

$nrRuntime = $null
foreach ($d in @($gameRoot, $targetDir, $addonDir)) {
    if (-not (Test-DirHere $d)) { continue }
    $p = Join-Safe $d 'nvngx_dlssnr.dll'
    if (Test-FileHere $p) { $nrRuntime = $p; break }
}
if ($nrRuntime) {
    $nrv = Get-FileVersionSafe $nrRuntime
    $script:Result.Static['NrRuntimeVersion'] = $nrv
    if (-not $nrv) { $nrv = 'unknown version' }
    Report -Status 'Ok' -Text ('nvngx_dlssnr.dll ' + $nrv)
}
else { Report -Status 'Warn' -Text 'nvngx_dlssnr.dll was not found beside the game.' }

$consumers = @()
foreach ($d in @($addonDir, $targetDir, $gameRoot, $remoteDir)) {
    if (-not (Test-DirHere $d)) { continue }
    foreach ($pat in @('renodx-dlss5*.addon64', 'dlss5-feed*.addon64', 'dlss5-feed*.addon32')) {
        foreach ($h in @(Get-ChildItem -LiteralPath $d -File -Filter $pat -ErrorAction SilentlyContinue)) { $consumers += $h.Name }
    }
    $opti = Join-Safe $d 'OptiScaler.ini'
    if (Test-FileHere $opti) {
        $t = Read-TextSafe $opti
        if ($t -and $t -match '(?im)^\s*\[DlssNr\]') { $consumers += 'OptiScaler.ini [DlssNr]' }
    }
}
$consumers = @($consumers | Sort-Object -Unique)
$script:Result.Static['Consumers'] = $consumers
if ($consumers.Count -gt 0) { Report -Status 'Ok' -Text ('Consumer: ' + ($consumers -join ', ')) }
else { Report -Status 'Warn' -Text 'No neural-rendering consumer found; nothing will call feature 18.' }

Write-Section 'Settings'

$script:Result.Static['PeripheralWarpSection'] = (Test-IniSection $iniText 'PeripheralWarp')
if (Test-IniSection $iniText 'PeripheralWarp') {
    $m = Get-IniKey $iniText 'PeripheralWarp' 'Mode'
    $mt = $m
    if (-not $mt) { $mt = '(unset)' }
    Report -Status 'Ok' -Text ('[PeripheralWarp] present, Mode=' + $mt)
    $script:Result.Static['IniMode'] = $m
    if ($m -eq '0') { Report -Status 'Warn' -Text 'Mode=0: the add-on is loaded but does nothing.' }
}
else { Report -Status 'Warn' -Text 'ReShade.ini has no [PeripheralWarp] section yet; the add-on writes one on its first run.' }

$disabledFound = @()
foreach ($pair in @(@{ Ini = $iniPath; Name = $script:AddonName }, @{ Ini = (Join-Safe $remoteDir 'ReShade.ini'); Name = $script:RemoteAddonName })) {
    if (-not $pair.Ini -or -not (Test-FileHere $pair.Ini)) { continue }
    $t = Read-TextSafe $pair.Ini
    $cur = Get-IniKey $t 'ADDON' 'DisabledAddons'
    if (-not $cur) { continue }
    foreach ($e in ($cur -split ',')) {
        $n = $e.Trim()
        if (-not $n) { continue }
        if ($n -ieq $pair.Name -or $n -match '(?i)^(Optimizer FPS for DLSS5\s+\d|PeripheralWarp Producer)') { $disabledFound += ($n + ' (' + (Get-RelativePathCompat $gameRoot $pair.Ini) + ')') }
    }
}
$script:Result.Static['DisabledAddons'] = @($disabledFound)
if ($disabledFound.Count -gt 0) {
    Report -Status 'Warn' -Text 'The add-on is listed in [ADDON] DisabledAddons; enable it in Home -> Add-ons.' -Detail ($disabledFound -join "`n")
}

$marker = Join-Safe $addonDir $script:SessionMarker
$script:Result.Static['StaleSessionMarker'] = (Test-FileHere $marker)
if (Test-FileHere $marker) {
    Report -Status 'Warn' -Text ($script:SessionMarker + ' is left over; the add-on will start in pass-through (crash guard).') `
           -Detail ('Delete ' + $marker + ', or press Retry in the add-on tab.')
}

# ---------------------------------------------------------------------------------------
# Runtime: what ReShade.log says
# ---------------------------------------------------------------------------------------

Write-Section 'Last run'

$logPath = Join-Safe $targetDir 'ReShade.log'
$logText = $null
$logStale = $false

if (-not (Test-FileHere $logPath)) {
    Report -Status 'Info' -Text 'No ReShade.log yet: the game has not run since the install.'
}
else {
    $logTime = (Get-Item -LiteralPath $logPath).LastWriteTimeUtc
    if ($receipt -and $receipt.PSObject.Properties['InstalledAt']) {
        $installedAt = [datetime]::MinValue
        try { $installedAt = ([datetime]::Parse([string]$receipt.InstalledAt)).ToUniversalTime() } catch { }
        if ($logTime -lt $installedAt) { $logStale = $true }
    }
    if ($logStale) {
        Report -Status 'Info' -Text 'ReShade.log is older than the install (stale log); start the game once.'
    }
    else { $logText = Read-TextSafe $logPath }
}

$rt = $script:Result.Runtime
$rt['LogPath']  = $logPath
$rt['LogStale'] = $logStale
$rt['Loaded']   = $false
$rt['Hooked']   = $false
$rt['Feature']  = $null
$rt['Warped']   = $false
$rt['Temporal'] = $null

$anyRuntime = $false

if ($logText) {
    $anyRuntime = $true

    if ($logText -match ('(?i)Registered add-on "' + [regex]::Escape($script:AddonName) + '"')) {
        $rt['Loaded'] = $true
        Report -Status 'Ok' -Text 'ReShade registered the add-on.'
    }
    else {
        Report -Status 'Fail' -Text 'ReShade never registered "Optimizer FPS for DLSS5".' `
               -Detail 'Either the add-on is disabled, or the ReShade build has no add-on support, or the file is blocked (mark of the web).'
    }

    if ($logText -match '(?i)nvngx_dlssnr\.dll feature 18 create/evaluate/release hooked') {
        $rt['Hooked'] = $true
        Report -Status 'Ok' -Text 'The NGX hook is in: nvngx_dlssnr.dll feature 18.'
    }
    else {
        Report -Status 'Warn' -Text 'The add-on never hooked nvngx_dlssnr.dll.' `
               -Detail 'Nothing loaded the neural-rendering runtime in that session: check the consumer and that DLSS 5 NR was on.'
    }

    $nativeW = 0; $nativeH = 0; $modelW = 0; $modelH = 0
    $m = [regex]::Match($logText, '(?i)feature 18 created, native (\d+)x(\d+), model (\d+)x(\d+) \(([^)]*)\)')
    if ($m.Success) {
        $nativeW = [int]$m.Groups[1].Value; $nativeH = [int]$m.Groups[2].Value
        $modelW  = [int]$m.Groups[3].Value; $modelH  = [int]$m.Groups[4].Value
        $why = $m.Groups[5].Value
        $rt['Feature'] = ('created ' + $nativeW + 'x' + $nativeH + ' -> ' + $modelW + 'x' + $modelH + ' (' + $why + ')')
        if ($why -match '(?i)pass-?through') {
            Report -Status 'Warn' -Text ('Feature 18 created at native ' + $nativeW + 'x' + $nativeH + ', but pass-through: ' + $why)
        }
        else {
            Report -Status 'Ok' -Text ('Feature 18: native ' + $nativeW + 'x' + $nativeH + ' -> model ' + $modelW + 'x' + $modelH + ' (' + $why + ')')
        }
    }
    else {
        $m = [regex]::Match($logText, '(?i)feature 18 adopted \(created before the hooks were installed\), native (\d+)x(\d+)')
        if ($m.Success) {
            $nativeW = [int]$m.Groups[1].Value; $nativeH = [int]$m.Groups[2].Value
            $rt['Feature'] = ('adopted ' + $nativeW + 'x' + $nativeH)
            Report -Status 'Ok' -Text ('Feature 18 adopted at native ' + $nativeW + 'x' + $nativeH + ' (it existed before the hooks went in).')
        }
        else {
            Report -Status 'Warn' -Text 'No feature 18 in the log: the consumer never asked for neural rendering.'
        }
    }

    $m = [regex]::Match($logText, '(?i)first warped evaluate completed \(model (\d+)x(\d+)\)')
    if (-not $m.Success) { $m = [regex]::Match($logText, '(?i)first warped evaluate completed \((?:D3D1[12]): \d+x\d+ -> packed (\d+)x(\d+)') }
    if ($m.Success) {
        $rt['Warped'] = $true
        $modelW = [int]$m.Groups[1].Value; $modelH = [int]$m.Groups[2].Value
        $rt['ModelWidth'] = $modelW
        $rt['ModelHeight'] = $modelH
        if ($nativeW -gt 0 -and $nativeH -gt 0) {
            $pct = 100.0 * ($modelW * $modelH) / ($nativeW * $nativeH)
            $rt['ModelPixelsPercent'] = [math]::Round($pct, 1)
            Report -Status 'Ok' -Text ('Frames are being warped: the model runs on ' + $modelW + 'x' + $modelH + ', ' + ('{0:N1}' -f $pct) + '% of the native pixels.')
        }
        else {
            Report -Status 'Ok' -Text ('Frames are being warped (model ' + $modelW + 'x' + $modelH + ').')
        }
    }
    else {
        Report -Status 'Warn' -Text 'No warped frame in the log yet.'
    }

    $m = [regex]::Match($logText, '(?i)temporal mode (\d+) running \(full pass every (\d+) frames')
    if ($m.Success) {
        $rt['Temporal'] = ('mode ' + $m.Groups[1].Value + ', full pass every ' + $m.Groups[2].Value + ' frames')
        Report -Status 'Ok' -Text ('Temporal mode ' + $m.Groups[1].Value + ' ran (full pass every ' + $m.Groups[2].Value + ' frames).')
    }
    $bg = [regex]::Matches($logText, '(?i)background pass (\d+) adopted').Count
    if ($bg -gt 0) {
        $rt['BackgroundPasses'] = $bg
        Report -Status 'Ok' -Text ('' + $bg + ' background model pass(es) adopted.')
    }

    if ($logText -match '(?i)shaders were not found in optimizer-fps-dlss5') {
        Report -Status 'Fail' -Text 'The add-on could not find its shaders in optimizer-fps-dlss5\ beside the add-on.'
    }
    $m = [regex]::Match($logText, '(?i)nvngx\.dll_optimizerfps\.dll (is missing beside the add-on|lacks its exports)')
    if ($m.Success) {
        Report -Status 'Fail' -Text ('The NGX forwarder ' + $m.Groups[1].Value + '.')
    }
    if ($logText -match '(?i)crash guard - the previous session of this game ended without unloading') {
        Report -Status 'Warn' -Text 'The crash guard tripped: that session forwarded everything untouched.' `
               -Detail 'Press Retry in the add-on tab, or delete optimizer-fps-dlss5.session, and start the game again.'
    }
}

# x86: the remote tab lives in the game's own ReShade.
if ($arch -eq 'x86' -and $remoteDir) {
    $remoteLog = Join-Safe $remoteDir 'ReShade.log'
    if (Test-FileHere $remoteLog) {
        $rlt = Read-TextSafe $remoteLog
        if ($rlt -and $rlt -match ('(?i)Registered add-on "' + [regex]::Escape($script:RemoteAddonName) + '"')) {
            $rt['RemoteTabLoaded'] = $true
            Report -Status 'Ok' -Text 'The 32-bit game loaded the remote tab.'
        }
        else {
            $rt['RemoteTabLoaded'] = $false
            Report -Status 'Warn' -Text 'The game''s own ReShade never registered the remote tab.'
        }
    }
    else { Report -Status 'Info' -Text 'No ReShade.log beside the 32-bit ReShade yet.' }
}

# ---------------------------------------------------------------------------------------

Write-Section 'Verdict'

if ($missing -gt 0 -or $script:CountFail -gt 0) {
    Write-Chunk ('  ' + $script:CountFail + ' failure(s), ' + $script:CountWarn + ' warning(s).') 'Red'
    Exit-Verify $script:ExitProblems
}
if (-not $anyRuntime -or -not $rt['Loaded']) {
    Write-Chunk '  Installed. Start the game once, then run Verify-OptimizerFPS.ps1 again.' 'Yellow'
    Exit-Verify $script:ExitNotVerified
}
Write-Chunk ('  Everything checks out (' + $script:CountWarn + ' warning(s)).') 'Green'
Exit-Verify $script:ExitOk

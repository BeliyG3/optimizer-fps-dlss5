#Requires -Version 5.1
<#
.SYNOPSIS
    Installs "Optimizer FPS for DLSS5" (the ReShade add-on) into a game.

.DESCRIPTION
    Copyright (c) 2026 Yuri Grib (BeliyG3). MIT licence (see LICENSE).

    Puts optimizer-fps-dlss5.addon64, its NGX forwarder and its compiled shaders beside the
    ReShade DLL a game already has, writes a starting [PeripheralWarp] block into
    ReShade.ini, keeps a receipt so the install can be undone exactly, and never touches
    anything it did not put there.

    32-bit games are installed through DLSS5-Feeder's 64-bit host (<game>\host64): the
    add-on lives there, and only the small remote tab goes beside the 32-bit ReShade DLL.

    Some helpers were adapted from the DLSS5-Feeder installer (MIT).

.PARAMETER GameExe
    The game's executable, or the folder that holds it. Required.

.PARAMETER Mode
    Install (default), Update (an Install that never rewrites an ini key that already
    exists), Verify (hand over to Verify-OptimizerFPS.ps1), Uninstall.

.PARAMETER Payload
    The payload folder. Defaults to <script folder>\payload.

.PARAMETER AllowDefenderExclusion
    Permission to offer a Windows Defender exclusion for the specific file(s) Defender
    removed. Never implied by -Yes: without this switch the installer only tells you what
    happened and leaves the decision to you.

.EXAMPLE
    .\Install-OptimizerFPS.ps1 "D:\Games\Baldurs Gate 3\bin\bg3_dx11.exe"

.EXAMPLE
    .\Install-OptimizerFPS.ps1 -GameExe "D:\Games\Witcher2" -Mode Update -Yes -NoPause
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string] $GameExe,

    [ValidateSet('Install', 'Verify', 'Uninstall', 'Update')]
    [string] $Mode = 'Install',

    [string] $Payload,

    [switch] $Yes,
    [switch] $NoPause,
    [switch] $NoVerify,
    [switch] $Force,
    [switch] $NoIni,
    [switch] $Json,
    [switch] $AllowDefenderExclusion
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------------------

$script:ExitOk            = 0
$script:ExitFail          = 1
$script:ExitGameRunning   = 2
$script:ExitNoReShade     = 3
$script:ExitReShadeOld    = 4
$script:ExitNoHost64      = 5
$script:ExitPayloadBad    = 6
$script:ExitNothingToDo   = 7
$script:ExitNotVerified   = 10

$script:ReShadeMinVersion = '6.8'
$script:ReShadeDllNames   = @('dxgi.dll', 'd3d11.dll', 'd3d12.dll', 'd3d9.dll', 'opengl32.dll', 'ReShade64.dll', 'ReShade32.dll')

$script:AddonName         = 'Optimizer FPS for DLSS5'
$script:RemoteAddonName   = 'Optimizer FPS for DLSS5 (tab for the 64-bit host)'

$script:AddonFileName     = 'optimizer-fps-dlss5.addon64'
$script:ForwarderFileName = 'nvngx.dll_optimizerfps.dll'
$script:RemoteFileName    = 'optimizer-fps-dlss5-remote.addon32'
$script:ShaderFolderName  = 'optimizer-fps-dlss5'
$script:SessionMarker     = 'optimizer-fps-dlss5.session'

# The names this add-on shipped under before 26.26, when the files still carried the SDK's
# name. They are ours: an Install or an Update backs them up, deletes them and puts the new
# names in their place -- a leftover .addon64 would otherwise keep being loaded as well.
$script:LegacyAddonFileName     = 'peripheral-warp.addon64'
$script:LegacyForwarderFileName = 'nvngx.dll_peripheralwarp.dll'
$script:LegacyRemoteFileName    = 'peripheral-warp-remote.addon32'
$script:LegacyShaderFolderName  = 'peripheral-warp'
$script:LegacySessionMarker     = 'peripheral-warp.session'
$script:StateFolderName   = '_OptimizerFPS'
$script:HostExeName       = 'dlss5-feed-host64.exe'
$script:ReceiptSchema     = 2

# The keys a fresh install seeds. Nothing else in [PeripheralWarp] is ever touched.
$script:DefaultIniKeys = @(
    @{ Key = 'Mode';    Value = '2'  },
    @{ Key = 'CenterX'; Value = '80' },
    @{ Key = 'CenterY'; Value = '80' },
    @{ Key = 'WorkX';   Value = '90' },
    @{ Key = 'WorkY';   Value = '90' }
)

# ---------------------------------------------------------------------------------------
# Output plumbing
# ---------------------------------------------------------------------------------------

$script:CountDone = 0
$script:CountWarn = 0
$script:CountFail = 0
$script:Manual    = New-Object System.Collections.ArrayList
$script:Changed   = New-Object System.Collections.ArrayList
$script:WarnLines = New-Object System.Collections.ArrayList
$script:FailLines = New-Object System.Collections.ArrayList

$script:Summary = [ordered]@{
    Tool                   = 'Install-OptimizerFPS'
    Mode                   = $Mode
    ExitCode               = 1
    Version                = $null
    Arch                   = $null
    GameExe                = $null
    TargetDir              = $null
    AddonDir               = $null
    RemoteDir              = $null
    IniPaths               = @()
    BackupDir              = $null
    ReceiptPath            = $null
    Files                  = @()
    IniKeysWritten         = @()
    DisabledAddonsMigrated = @()
    DefenderExclusions     = @()
    RemovedLegacyFiles     = @()
    RemovedStaleShaders    = @()
    Warnings               = @()
    Failures               = @()
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
    if ($Json) { return }
    try {
        if ($script:UseColour -and $Colour) { Write-Host $Text -ForegroundColor $Colour -NoNewline:$NoNewline }
        else { Write-Host $Text -NoNewline:$NoNewline }
    }
    catch {
        $script:UseColour = $false
        Write-Host $Text -NoNewline:$NoNewline
    }
}

function Write-Line
{
    param([string] $Text)
    if ($Json) { return }
    Write-Host $Text
}

function Write-Section
{
    param([string] $Title)
    if ($Json) { return }
    Write-Host ''
    Write-Chunk ('  ' + [char]0x2500 + [char]0x2500 + ' ') 'Green' -NoNewline
    Write-Chunk $Title 'White' -NoNewline
    $pad = 62 - $Title.Length
    if ($pad -lt 1) { $pad = 1 }
    Write-Chunk (' ' + ([string][char]0x2500) * $pad) 'Green'
}

function Write-Banner
{
    if ($Json) { return }
    $box = 'DarkGray'
    $w   = 68
    Write-Host ''
    Write-Chunk ('  ' + [char]0x2554 + ([string][char]0x2550) * $w + [char]0x2557) $box
    $rows = @(
        @{ Text = '  Optimizer FPS for DLSS5'; Colour = 'White' },
        @{ Text = '  the frame periphery, compressed before DLSS Neural Rendering'; Colour = 'DarkGray' }
    )
    foreach ($r in $rows) {
        Write-Chunk ('  ' + [char]0x2551) $box -NoNewline
        Write-Chunk $r.Text $r.Colour -NoNewline
        $used = $r.Text.Length
        if ($used -lt $w) { Write-Chunk ((' ') * ($w - $used)) $null -NoNewline }
        Write-Chunk ([string][char]0x2551) $box
    }
    Write-Chunk ('  ' + [char]0x255A + ([string][char]0x2550) * $w + [char]0x255D) $box
}

# $Status: Done (something was installed/written), Ok (already right, nothing to do),
# Skip (not applicable), Warn, Fail, Info.
function Report
{
    param(
        [ValidateSet('Done', 'Ok', 'Skip', 'Warn', 'Fail', 'Info')]
        [string] $Status,
        [string] $Text,
        [string] $Detail,
        [string] $Manual
    )

    $glyph  = '[ .. ]'
    $colour = 'DarkGray'
    switch ($Status) {
        'Done' { $glyph = '[DONE]'; $colour = 'Green';    $script:CountDone++ }
        'Ok'   { $glyph = '[ OK ]'; $colour = 'Green' }
        'Skip' { $glyph = '[ -- ]'; $colour = 'DarkGray' }
        'Warn' { $glyph = '[WARN]'; $colour = 'Yellow';   $script:CountWarn++; $null = $script:WarnLines.Add($Text) }
        'Fail' { $glyph = '[FAIL]'; $colour = 'Red';      $script:CountFail++; $null = $script:FailLines.Add($Text) }
        'Info' { $glyph = '[ .. ]'; $colour = 'DarkGray' }
    }

    if ($Manual) { $null = $script:Manual.Add($Manual) }
    if ($Json) { return }

    Write-Chunk ('  ' + $glyph + ' ') $colour -NoNewline
    if ($Status -eq 'Skip' -or $Status -eq 'Info') { Write-Chunk $Text 'DarkGray' } else { Write-Host $Text }
    if ($Detail) {
        foreach ($line in ($Detail -split "`n")) {
            if ($line.Trim()) { Write-Chunk ('         ' + $line.Trim()) 'DarkGray' }
        }
    }
    if ($Manual) {
        $first = $true
        foreach ($line in ($Manual -split "`n")) {
            if (-not $line.Trim()) { continue }
            if ($first) { Write-Chunk ('         ' + [char]0x2192 + ' ' + $line.Trim()) 'DarkYellow'; $first = $false }
            else { Write-Chunk ('           ' + $line.Trim()) 'DarkYellow' }
        }
    }
}

function Exit-Installer
{
    param([int] $Code)

    if ($script:Lock) { try { $script:Lock.ReleaseMutex() } catch { } ; try { $script:Lock.Dispose() } catch { } ; $script:Lock = $null }

    if ($Json) {
        $script:Summary.ExitCode = $Code
        $script:Summary.Warnings = @($script:WarnLines.ToArray())
        $script:Summary.Failures = @($script:FailLines.ToArray())
        Write-Output ($script:Summary | ConvertTo-Json -Depth 12)
    }
    if (-not $NoPause) {
        Write-Host ''
        try { [void](Read-Host '  Press Enter to exit') } catch { }
    }
    exit $Code
}

function Stop-Install
{
    param([string] $Text, [string] $Detail, [string] $Manual, [int] $Code = 1)
    Report -Status 'Fail' -Text $Text -Detail $Detail -Manual $Manual
    Write-Line ''
    Write-Chunk '  Stopped: nothing further was changed.' 'Red'
    Exit-Installer $Code
}

function Confirm-Step
{
    param([string] $Question)
    if ($Yes) { return $true }
    if ($Json) { return $false }
    Write-Host ''
    Write-Chunk ('  ' + $Question + ' [y/N] ') 'Cyan' -NoNewline
    try { $a = Read-Host } catch { return $false }
    return ($a -match '^(?i)y(es)?$')
}

# ---------------------------------------------------------------------------------------
# Small, defensive helpers
# ---------------------------------------------------------------------------------------

function Join-Safe
{
    param([string] $Parent, [string] $Child)
    try { return [IO.Path]::Combine($Parent, $Child) } catch { return $null }
}

function Test-FileHere
{
    param([string] $Path)
    if (-not $Path) { return $false }
    try { return (Test-Path -LiteralPath $Path -PathType Leaf) } catch { return $false }
}

function Test-DirHere
{
    param([string] $Path)
    if (-not $Path) { return $false }
    try { return (Test-Path -LiteralPath $Path -PathType Container) } catch { return $false }
}

function New-DirSafe
{
    param([string] $Path)
    if (-not (Test-DirHere $Path)) { $null = New-Item -ItemType Directory -Path $Path -Force }
}

function Get-Sha256
{
    param([string] $Path)
    try { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash.ToUpperInvariant() } catch { return $null }
}

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

function Get-ProductNameSafe
{
    param([string] $Path)
    if (-not (Test-FileHere $Path)) { return $null }
    try {
        $vi = (Get-Item -LiteralPath $Path -ErrorAction Stop).VersionInfo
        $bits = @()
        if ($vi.ProductName)     { $bits += $vi.ProductName }
        if ($vi.FileDescription) { $bits += $vi.FileDescription }
        if ($vi.InternalName)    { $bits += $vi.InternalName }
        return ($bits -join ' | ')
    }
    catch { }
    return $null
}

# Scan a binary for an ASCII marker. Capped: never slurp a 160 MB NGX runtime.
function Get-BinaryMarker
{
    param([string] $Path, [string] $Pattern, [int] $MaxBytes = 33554432)
    if (-not (Test-FileHere $Path)) { return $null }
    try {
        $fi = Get-Item -LiteralPath $Path -ErrorAction Stop
        if ($fi.Length -gt $MaxBytes -or $fi.Length -eq 0) { return $null }
        $bytes = [IO.File]::ReadAllBytes($Path)
        $m = [regex]::Match([Text.Encoding]::ASCII.GetString($bytes), $Pattern)
        if ($m.Success) {
            if ($m.Groups.Count -gt 1 -and $m.Groups[1].Success) { return $m.Groups[1].Value }
            return $m.Value
        }
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

# [IO.Path]::GetRelativePath is .NET Core only; 5.1 has to do it by hand.
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

# ---------------------------------------------------------------------------------------
# Atomic writes and the per-folder lock
# ---------------------------------------------------------------------------------------

function Copy-FileAtomic
{
    param([string] $Source, [string] $Destination, [string] $ExpectedSha256)
    $parent = Split-Path -Parent $Destination
    New-DirSafe $parent
    $temporary = Join-Safe $parent ('.optimizerfps-copy-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        Copy-Item -LiteralPath $Source -Destination $temporary -Force
        if ($ExpectedSha256) {
            $got = Get-Sha256 $temporary
            if (-not [string]::Equals($got, $ExpectedSha256, [StringComparison]::OrdinalIgnoreCase)) {
                throw ('Copied file hash mismatch: ' + $Destination)
            }
        }
        Move-Item -LiteralPath $temporary -Destination $Destination -Force
    }
    finally {
        if (Test-Path -LiteralPath $temporary) { try { Remove-Item -LiteralPath $temporary -Force } catch { } }
    }
    $null = $script:Changed.Add($Destination)
}

function Write-TextAtomic
{
    param([string] $Content, [string] $Path)
    $parent = Split-Path -Parent $Path
    New-DirSafe $parent
    $temporary = Join-Safe $parent ('.optimizerfps-write-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [IO.File]::WriteAllText($temporary, $Content, (New-Object Text.UTF8Encoding($false)))
        Move-Item -LiteralPath $temporary -Destination $Path -Force
    }
    finally {
        if (Test-Path -LiteralPath $temporary) { try { Remove-Item -LiteralPath $temporary -Force } catch { } }
    }
    $null = $script:Changed.Add($Path)
}

function Write-JsonAtomic
{
    param($Value, [string] $Path)
    Write-TextAtomic -Content ($Value | ConvertTo-Json -Depth 32) -Path $Path
}

$script:Lock = $null

function Enter-InstallLock
{
    param([string] $Folder)
    $key = $Folder.ToLowerInvariant()
    $md5 = $null
    try {
        $md5 = [Security.Cryptography.MD5]::Create()
        $bytes = $md5.ComputeHash([Text.Encoding]::Unicode.GetBytes($key))
        $hex = [BitConverter]::ToString($bytes).Replace('-', '')
    }
    finally { if ($md5) { try { $md5.Dispose() } catch { } } }

    $mutex = New-Object System.Threading.Mutex($false, ('Local\OptimizerFPS-' + $hex))
    $owned = $false
    try { $owned = $mutex.WaitOne(0) }
    catch [System.Threading.AbandonedMutexException] { $owned = $true }
    if (-not $owned) {
        try { $mutex.Dispose() } catch { }
        Stop-Install -Text 'Another Optimizer FPS install is already running for this game folder.' `
                     -Detail $Folder
    }
    $script:Lock = $mutex
}

# ---------------------------------------------------------------------------------------
# PE parsing: architecture, imports, export names.
# Opened FileShare.ReadWrite so a running game does not block the read.
# ---------------------------------------------------------------------------------------

function Get-PeInfo
{
    param([string] $Path)
    if (-not (Test-FileHere $Path)) { return $null }

    $fs = $null
    $br = $null
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

        $machine   = $br.ReadUInt16()
        $nSections = $br.ReadUInt16()
        $null      = $br.ReadUInt32()   # timestamp
        $null      = $br.ReadUInt32()   # symbol table
        $null      = $br.ReadUInt32()   # symbol count
        $optSize   = $br.ReadUInt16()
        $null      = $br.ReadUInt16()   # characteristics

        $bits = 0
        $arch = 'unknown'
        switch ($machine) {
            0x014C  { $bits = 32; $arch = 'x86' }
            0x8664  { $bits = 64; $arch = 'x64' }
            0xAA64  { $bits = 64; $arch = 'ARM64' }
            0x01C4  { $bits = 32; $arch = 'ARM' }
            default { $bits = 0;  $arch = ('unknown machine 0x{0:X4}' -f $machine) }
        }

        $imports = @()
        try {
            $optOff = $peOff + 24
            $fs.Position = $optOff
            $magic = $br.ReadUInt16()
            $ddOff = 0
            if ($magic -eq 0x10B) { $ddOff = $optOff + 96 } elseif ($magic -eq 0x20B) { $ddOff = $optOff + 112 }

            if ($ddOff -gt 0) {
                $fs.Position = $ddOff + 8     # data directory [1] = imports
                $impRva  = $br.ReadUInt32()
                $impSize = $br.ReadUInt32()

                $sections = @()
                $secOff = $optOff + $optSize
                for ($i = 0; $i -lt $nSections; $i++) {
                    $fs.Position = $secOff + $i * 40 + 8
                    $vsize = $br.ReadUInt32(); $vaddr = $br.ReadUInt32(); $rsize = $br.ReadUInt32(); $rptr = $br.ReadUInt32()
                    $span = [math]::Max($vsize, $rsize)
                    $sections += New-Object psobject -Property @{ V = $vaddr; Span = $span; Raw = $rptr }
                }

                if ($impRva -gt 0 -and $impSize -gt 0) {
                    $descOff = Convert-RvaToOffset $sections $impRva $fs.Length
                    $n = 0
                    while ($descOff -ge 0 -and ($descOff + 20) -lt $fs.Length -and $n -lt 512) {
                        $fs.Position = $descOff + 12
                        $nameRva = $br.ReadUInt32()
                        if ($nameRva -eq 0) { break }
                        $nameOff = Convert-RvaToOffset $sections $nameRva $fs.Length
                        if ($nameOff -ge 0 -and $nameOff -lt $fs.Length) {
                            $fs.Position = $nameOff
                            $sb = New-Object Text.StringBuilder
                            for ($k = 0; $k -lt 260; $k++) {
                                $b = $fs.ReadByte()
                                if ($b -le 0) { break }
                                $null = $sb.Append([char]$b)
                            }
                            if ($sb.Length -gt 0) { $imports += $sb.ToString() }
                        }
                        $descOff += 20
                        $n++
                    }
                }
            }
        }
        catch { }

        return New-Object psobject -Property @{ Bits = $bits; Arch = $arch; Imports = $imports }
    }
    catch { return $null }
    finally {
        if ($br) { try { $br.Close() } catch { } }
        if ($fs) { try { $fs.Dispose() } catch { } }
    }
}

# RVA -> file offset, given a PE's section table.
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

# The names in a PE's export directory. $null when the file is not a readable PE; an empty
# array when it is one and exports nothing.
function Get-PeExportNames
{
    param([string] $Path, [int] $Max = 8192)
    if (-not (Test-FileHere $Path)) { return $null }

    $fs = $null
    $br = $null
    try {
        $fs = New-Object IO.FileStream($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        if ($fs.Length -lt 0x40) { return $null }
        $br = New-Object IO.BinaryReader($fs)

        if ($br.ReadUInt16() -ne 0x5A4D) { return $null }            # 'MZ'
        $fs.Position = 0x3C
        $peOff = $br.ReadInt32()
        if ($peOff -le 0 -or ($peOff + 24) -ge $fs.Length) { return $null }

        $fs.Position = $peOff
        if ($br.ReadUInt32() -ne 0x00004550) { return $null }        # 'PE\0\0'
        $null      = $br.ReadUInt16()                                # Machine
        $nSections = $br.ReadUInt16()
        $null      = $br.ReadUInt32()                                # TimeDateStamp
        $null      = $br.ReadUInt32()                                # PointerToSymbolTable
        $null      = $br.ReadUInt32()                                # NumberOfSymbols
        $optSize   = $br.ReadUInt16()
        $null      = $br.ReadUInt16()                                # Characteristics

        $optOff = [long]$peOff + 24
        if ($optSize -lt 96 -or ($optOff + $optSize) -gt $fs.Length) { return $null }
        $fs.Position = $optOff
        $magic = $br.ReadUInt16()
        # The data directories follow the optional header's fixed part: 96 bytes for PE32,
        # 112 for PE32+ (four fields widened to 64-bit).
        $dirOff = 0
        if     ($magic -eq 0x20B) { $dirOff = $optOff + 112 }
        elseif ($magic -eq 0x10B) { $dirOff = $optOff + 96 }
        else                      { return $null }
        if (($dirOff + 8) -gt $fs.Length) { return $null }
        $fs.Position = $dirOff
        $expRva = $br.ReadUInt32()                                   # DataDirectory[0] = exports
        $null   = $br.ReadUInt32()                                   # its size
        if ($expRva -eq 0) { return ,([string[]] @()) }

        $secOff = $optOff + $optSize
        $sections = @()
        for ($i = 0; $i -lt $nSections; $i++) {
            $p = $secOff + ([long]$i * 40)
            if (($p + 40) -gt $fs.Length) { break }
            $fs.Position = $p + 8                                    # past the 8-byte name
            $vsize = $br.ReadUInt32()
            $vaddr = $br.ReadUInt32()
            $rsize = $br.ReadUInt32()
            $raw   = $br.ReadUInt32()
            $span  = $rsize
            if ($vsize -gt 0) { $span = $vsize }
            $sections += New-Object psobject -Property @{ V = $vaddr; Span = $span; Raw = $raw }
        }
        if ($sections.Count -eq 0) { return $null }

        $eo = Convert-RvaToOffset $sections $expRva $fs.Length
        if ($eo -lt 0 -or ($eo + 40) -gt $fs.Length) { return $null }

        # IMAGE_EXPORT_DIRECTORY: NumberOfNames +24, AddressOfNames +32.
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

# ReShade's own add-on API finds the ReShade module by testing GetProcAddress for exactly
# "ReShadeRegisterAddon" and "ReShadeUnregisterAddon" (reshade.hpp). A build without them
# cannot load an add-on at all. $null means the file could not be read.
function Test-ReShadeHasAddons
{
    param([string] $Path)
    $names = Get-PeExportNames $Path
    if ($null -eq $names) { return $null }
    return [bool](($names -contains 'ReShadeRegisterAddon') -and ($names -contains 'ReShadeUnregisterAddon'))
}

function Test-ReShadeVersionOk
{
    param([string] $Version)
    if (-not $Version) { return $null }
    $m = [regex]::Match($Version, '^\s*(\d+)\.(\d+)')
    if (-not $m.Success) { return $null }
    $want = $script:ReShadeMinVersion -split '\.'
    $maj = [int]$m.Groups[1].Value; $min = [int]$m.Groups[2].Value
    if ($maj -gt [int]$want[0]) { return $true }
    if ($maj -eq [int]$want[0] -and $min -ge [int]$want[1]) { return $true }
    return $false
}

function Test-IsReShade
{
    param([string] $Path)
    $n = Get-ProductNameSafe $Path
    if ($n -and $n -match '(?i)reshade') { return $true }
    return [bool]((Test-ReShadeHasAddons $Path) -eq $true)
}

# ---------------------------------------------------------------------------------------
# Ini editing that keeps everything else in the file intact
# ---------------------------------------------------------------------------------------

function Set-IniKey
{
    param([string] $Text, [string] $Section, [string] $Key, [string] $Value)

    if ($null -eq $Text) { $Text = '' }
    $nl = "`r`n"
    $lines = New-Object System.Collections.ArrayList
    foreach ($l in ($Text -split "`r?`n")) { $null = $lines.Add($l) }
    if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }

    $cur = ''
    $secStart = -1
    $secEnd = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^\[(.+)\]$') {
            if ($cur -ieq $Section -and $secStart -ge 0 -and $secEnd -lt 0) { $secEnd = $i }
            $cur = $Matches[1]
            if ($cur -ieq $Section) { $secStart = $i + 1 }
            continue
        }
        if ($cur -ieq $Section -and $t -match ('(?i)^' + [regex]::Escape($Key) + '\s*=')) {
            $lines[$i] = $Key + '=' + $Value
            return (($lines -join $nl) + $nl)
        }
    }
    if ($Section -eq '' -and $secStart -lt 0) {
        $secStart = 0; $secEnd = 0
        for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i].Trim() -match '^\[') { break }; $secEnd = $i + 1 }
    }
    if ($secStart -ge 0) {
        if ($secEnd -lt 0) { $secEnd = $lines.Count }
        $at = $secEnd
        while ($at -gt $secStart -and $lines[$at - 1].Trim() -eq '') { $at-- }
        $lines.Insert($at, ($Key + '=' + $Value))
    }
    else {
        if ($lines.Count -gt 0) { $null = $lines.Add('') }
        $null = $lines.Add('[' + $Section + ']')
        $null = $lines.Add($Key + '=' + $Value)
    }
    return (($lines -join $nl) + $nl)
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

function Remove-IniKey
{
    param([string] $Text, [string] $Section, [string] $Key)
    if ($null -eq $Text) { return $null }
    $nl = "`r`n"
    $lines = New-Object System.Collections.ArrayList
    foreach ($l in ($Text -split "`r?`n")) { $null = $lines.Add($l) }
    if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }

    $cur = ''
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^\[(.+)\]$') { $cur = $Matches[1]; continue }
        if ($cur -ieq $Section -and $t -match ('(?i)^' + [regex]::Escape($Key) + '\s*=')) {
            $lines.RemoveAt($i)
            return (($lines -join $nl) + $nl)
        }
    }
    return (($lines -join $nl) + $nl)
}

# Drops "[Section]" when nothing but blank lines follows it.
function Remove-EmptyIniSection
{
    param([string] $Text, [string] $Section)
    if ($null -eq $Text) { return $null }
    $nl = "`r`n"
    $lines = New-Object System.Collections.ArrayList
    foreach ($l in ($Text -split "`r?`n")) { $null = $lines.Add($l) }
    if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }

    $start = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^\[(.+)\]$' -and $Matches[1] -ieq $Section) { $start = $i; break }
    }
    if ($start -lt 0) { return (($lines -join $nl) + $nl) }

    $end = $lines.Count
    for ($i = $start + 1; $i -lt $lines.Count; $i++) {
        if ($lines[$i].Trim() -match '^\[.+\]$') { $end = $i; break }
    }
    for ($i = $start + 1; $i -lt $end; $i++) {
        if ($lines[$i].Trim() -ne '') { return (($lines -join $nl) + $nl) }
    }
    # Take the blank lines that were inserted in front of the header with it, so removing a
    # section this installer added leaves the file exactly as it was before.
    $from = $start
    while ($from -gt 0 -and $lines[$from - 1].Trim() -eq '') { $from-- }
    for ($i = $end - 1; $i -ge $from; $i--) { $lines.RemoveAt($i) }
    return (($lines -join $nl) + $nl)
}

# ---------------------------------------------------------------------------------------
# ReShade's DisabledAddons is a comma-separated list of add-on NAMEs. Older builds of this
# add-on carried the release number inside the name, so a user's "disabled" choice has to
# be carried over to the new, version-free name instead of silently re-enabling it.
# ---------------------------------------------------------------------------------------

function Convert-AddonNameToCurrent
{
    param([string] $Entry)
    $e = $Entry.Trim()
    if ($e -match '(?i)^Optimizer FPS for DLSS5\s+\d+(\.\d+)+\s*\(tab for the 64-bit host\)$') { return $script:RemoteAddonName }
    if ($e -match '(?i)^Optimizer FPS for DLSS5\s+\d+(\.\d+)+$')                               { return $script:AddonName }
    if ($e -match '(?i)^PeripheralWarp Producer\b.*\(tab for the 64-bit host\)$')              { return $script:RemoteAddonName }
    if ($e -match '(?i)^PeripheralWarp Producer\b')                                            { return $script:AddonName }
    return $e
}

# ---------------------------------------------------------------------------------------
# Elevation and Windows Defender
# ---------------------------------------------------------------------------------------

function Test-Elevated
{
    try {
        $id = [Security.Principal.WindowsIdentity]::GetCurrent()
        return (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    }
    catch { return $false }
}

function ConvertTo-PsLiteral
{
    param([string] $S)
    return ("'" + ($S -replace "'", "''") + "'")
}

function Invoke-Elevated
{
    param([string] $Script, [string] $What)

    $body = "`$ErrorActionPreference = 'Stop'`r`ntry {`r`n" + $Script + "`r`n  exit 0`r`n}`r`ncatch {`r`n  Write-Host `$_.Exception.Message`r`n  Start-Sleep -Seconds 4`r`n  exit 1`r`n}`r`n"
    $tmp = Join-Safe ([IO.Path]::GetTempPath()) ('optimizerfps-elevated-' + [Guid]::NewGuid().ToString('N') + '.ps1')
    [IO.File]::WriteAllText($tmp, $body, (New-Object Text.UTF8Encoding($true)))

    $code = 1
    if (Test-Elevated) {
        try { & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $tmp; $code = $LASTEXITCODE }
        catch { $code = 1 }
    }
    else {
        Write-Chunk ('  [ .. ] Asking for administrator rights: ' + $What) 'Cyan'
        try {
            $p = Start-Process -FilePath 'powershell.exe' -Verb RunAs -Wait -PassThru `
                     -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $tmp + '"'))
            $code = $p.ExitCode
        }
        catch {
            try { Remove-Item -LiteralPath $tmp -Force } catch { }
            return 'declined'
        }
    }
    try { Remove-Item -LiteralPath $tmp -Force } catch { }
    if ($code -eq 0) { return 'ok' }
    return 'failed'
}

# A detection in the last 15 minutes naming $Path -- or $null.
function Get-DefenderDetection
{
    param([string] $Path)
    try {
        $since = (Get-Date).AddMinutes(-15)
        $all = Get-MpThreatDetection -ErrorAction Stop
        foreach ($d in $all) {
            if ($d.InitialDetectionTime -lt $since) { continue }
            $res = @($d.Resources) -join ' '
            if ($res -match [regex]::Escape($Path)) { return $d }
        }
    }
    catch { }
    return $null
}

$script:DefenderExcluded  = @{}
$script:DefenderExclusions = New-Object System.Collections.ArrayList

# An exclusion is never automatic: it weakens the user's antivirus, so it needs its own
# explicit -AllowDefenderExclusion switch. -Yes answers the installer's own questions and
# deliberately does not answer this one. Only the exact file(s) Defender removed are
# excluded -- never the folder they sit in.
function Request-DefenderExclusion
{
    param([string[]] $Paths, [string] $File)

    $todo = @()
    foreach ($p in $Paths) { if (-not $script:DefenderExcluded.ContainsKey($p.ToLowerInvariant())) { $todo += $p } }
    if ($todo.Count -eq 0) { return $true }

    $cmd = ($todo | ForEach-Object { 'Add-MpPreference -ExclusionPath ' + (ConvertTo-PsLiteral $_) }) -join '; '

    Write-Line ''
    Write-Chunk ('  Windows Defender flagged ' + $File + '.') 'Yellow'
    Write-Chunk '  The add-on uses Microsoft Detours to hook DLSS functions, which some AV heuristics' 'DarkGray'
    Write-Chunk '  flag; if you trust this download, add an exclusion for the listed file(s) only:' 'DarkGray'
    foreach ($p in $todo) { Write-Chunk ('      ' + $p) 'White' }

    if (-not $AllowDefenderExclusion) {
        Report -Status 'Fail' -Text 'Defender removed a file this install needs; no exclusion was added.' `
               -Manual ('Add it yourself (elevated PowerShell): ' + $cmd + "`n" +
                        'or re-run this installer with -AllowDefenderExclusion, which asks the same question ' +
                        'and adds the exclusion for you.')
        return $false
    }

    if (-not (Confirm-Step 'Add the Defender exclusion for the file(s) above now?')) {
        Report -Status 'Fail' -Text 'Defender exclusion declined.' `
               -Manual ('Add it yourself (elevated PowerShell): ' + $cmd + "`nThen re-run this installer.")
        return $false
    }

    $lines = ($todo | ForEach-Object { '  Add-MpPreference -ExclusionPath ' + (ConvertTo-PsLiteral $_) }) -join "`r`n"
    $r = Invoke-Elevated -Script $lines -What 'add a Windows Defender exclusion'
    if ($r -eq 'ok') {
        foreach ($p in $todo) {
            $script:DefenderExcluded[$p.ToLowerInvariant()] = $true
            $null = $script:DefenderExclusions.Add($p)
        }
        Report -Status 'Done' -Text 'Defender exclusion added for the listed file(s).'
        return $true
    }
    Report -Status 'Fail' -Text ('Defender exclusion could not be added (' + $r + ').')
    return $false
}

# Undo of the above, at uninstall time.
function Remove-DefenderExclusion
{
    param([string[]] $Paths)

    $todo = @()
    foreach ($p in $Paths) { if ($p) { $todo += [string]$p } }
    if ($todo.Count -eq 0) { return }

    $lines = ($todo | ForEach-Object { '  Remove-MpPreference -ExclusionPath ' + (ConvertTo-PsLiteral $_) }) -join "`r`n"
    $r = Invoke-Elevated -Script $lines -What 'remove the Windows Defender exclusion this installer added'
    if ($r -eq 'ok') { Report -Status 'Done' -Text ('Removed ' + $todo.Count + ' Defender exclusion(s) this installer had added.') }
    else {
        $cmd = ($todo | ForEach-Object { 'Remove-MpPreference -ExclusionPath ' + (ConvertTo-PsLiteral $_) }) -join '; '
        Report -Status 'Warn' -Text ('Could not remove the Defender exclusion(s) (' + $r + ').') `
               -Detail ('Remove them yourself (elevated PowerShell): ' + $cmd)
    }
}

# ---------------------------------------------------------------------------------------
# Recognising our own files
# ---------------------------------------------------------------------------------------

# $Kind: addon64 | forwarder | addon32 | dxbc
function Test-IsOurFile
{
    param([string] $Path, [string] $Kind)
    if (-not (Test-FileHere $Path)) { return $false }
    switch ($Kind) {
        'addon64' {
            $n = Get-PeExportNames $Path
            if ($null -eq $n) { return $false }
            return [bool](($n -contains 'PeripheralWarpSetLayoutV1') -or ($n -contains 'PeripheralWarpSetTemporalV1'))
        }
        'forwarder' {
            $leaf = [IO.Path]::GetFileName($Path)
            if ($leaf -ine $script:ForwarderFileName -and $leaf -ine $script:LegacyForwarderFileName) { return $false }
            $n = Get-PeExportNames $Path
            if ($null -eq $n) { return $false }
            return [bool](($n -contains 'pw_ngx_call_create') -and ($n -contains 'pw_ngx_call_evaluate'))
        }
        'addon32' {
            $n = Get-PeExportNames $Path
            if ($null -ne $n -and ($n -contains 'PeripheralWarpSetLayoutV1')) { return $true }
            $m = Get-BinaryMarker $Path '(?i)(Optimizer FPS for DLSS5|PeripheralWarp)'
            return [bool]$m
        }
        'dxbc' { return $true }   # ours by location: it sits in the add-on's optimizer-fps-dlss5\ folder
    }
    return $false
}

function Get-PayloadKind
{
    param([string] $Relative)
    $r = $Relative.Replace('\', '/')
    $leaf = [IO.Path]::GetFileName($r)
    if ($leaf -ieq $script:ForwarderFileName -or $leaf -ieq $script:LegacyForwarderFileName) { return 'forwarder' }
    if ($leaf -ieq $script:RemoteFileName -or $leaf -ieq $script:LegacyRemoteFileName)       { return 'addon32' }
    if ($r -match '(?i)\.dxbc$')              { return 'dxbc' }
    if ($r -match '(?i)\.addon64$')           { return 'addon64' }
    return 'other'
}

# Files from a release older than 26.26, when the shipped names still read "peripheral-warp".
# Anything recognisably ours is backed up and deleted; anything else keeps its name and is
# only reported. Returns the paths that were removed.
function Remove-LegacyInstall
{
    param([string] $AddonDir, [string] $RemoteDir, [string] $BackupDir, [string] $TargetDir)

    $removed = New-Object System.Collections.ArrayList
    $candidates = New-Object System.Collections.ArrayList

    foreach ($n in @($script:LegacyAddonFileName, $script:LegacyForwarderFileName)) {
        $p = Join-Safe $AddonDir $n
        $kind = 'addon64'
        if ($n -ieq $script:LegacyForwarderFileName) { $kind = 'forwarder' }
        if (Test-FileHere $p) { $null = $candidates.Add(@{ Path = $p; Kind = $kind }) }
    }
    $legacyShaderDir = Join-Safe $AddonDir $script:LegacyShaderFolderName
    if (Test-DirHere $legacyShaderDir) {
        foreach ($h in @(Get-ChildItem -LiteralPath $legacyShaderDir -File -Filter '*.dxbc' -ErrorAction SilentlyContinue)) {
            $null = $candidates.Add(@{ Path = $h.FullName; Kind = 'dxbc' })
        }
    }
    if ($RemoteDir) {
        $p = Join-Safe $RemoteDir $script:LegacyRemoteFileName
        if (Test-FileHere $p) { $null = $candidates.Add(@{ Path = $p; Kind = 'addon32' }) }
    }

    foreach ($c in $candidates) {
        if (-not (Test-IsOurFile $c.Path $c.Kind)) {
            Report -Status 'Warn' -Text ('A file with a pre-26.26 name is there but is not ours; left alone: ' + $c.Path)
            continue
        }
        $rel = Get-RelativePathCompat $TargetDir $c.Path
        $safeRel = $rel -replace '^[A-Za-z]:\\', '' -replace '^\\\\', ''
        try {
            Copy-FileAtomic -Source $c.Path -Destination (Join-Safe $BackupDir ('legacy\' + $safeRel))
            Remove-Item -LiteralPath $c.Path -Force
            $null = $removed.Add($c.Path)
        }
        catch { Report -Status 'Warn' -Text ('Could not remove the old-name file ' + $c.Path) -Detail $_.Exception.Message }
    }

    # The old shader folder goes with its last file.
    if (Test-DirHere $legacyShaderDir) {
        $left = @(Get-ChildItem -LiteralPath $legacyShaderDir -Force -ErrorAction SilentlyContinue)
        if ($left.Count -eq 0) { try { Remove-Item -LiteralPath $legacyShaderDir -Force } catch { } }
    }

    # ... and so does a crash-guard marker under the old name.
    $legacyMarker = Join-Safe $AddonDir $script:LegacySessionMarker
    if (Test-FileHere $legacyMarker) {
        try { Remove-Item -LiteralPath $legacyMarker -Force; $null = $removed.Add($legacyMarker) } catch { }
    }

    return @($removed.ToArray())
}

# ---------------------------------------------------------------------------------------
# Start
# ---------------------------------------------------------------------------------------

Write-Banner

if (-not $Payload) { $Payload = Join-Path $PSScriptRoot 'payload' }

if (-not $GameExe) {
    Report -Status 'Fail' -Text 'No game given.' `
        -Detail ("Usage: Install-OptimizerFPS.ps1 <game .exe or game folder> [-Mode Install|Update|Verify|Uninstall]`n" +
                 "                                 [-Payload <dir>] [-Yes] [-NoPause] [-NoVerify] [-Force] [-NoIni] [-Json]")
    Exit-Installer $script:ExitFail
}

$verifyScript = Join-Path $PSScriptRoot 'Verify-OptimizerFPS.ps1'

if ($Mode -eq 'Verify') {
    if (-not (Test-FileHere $verifyScript)) {
        Stop-Install -Text 'Verify-OptimizerFPS.ps1 is not beside this script.' -Detail $verifyScript
    }
    $psExe = 'powershell.exe'
    try { $psExe = [Diagnostics.Process]::GetCurrentProcess().MainModule.FileName } catch { }
    $vArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $verifyScript, '-GameExe', $GameExe)
    if ($Json) { $vArgs += '-Json' }
    & $psExe @vArgs
    $code = $LASTEXITCODE
    if ($null -eq $code) { $code = 0 }
    exit $code
}

# --- 1. Resolve the game ---------------------------------------------------------------

Write-Section 'Game'

$gamePath = $null
try { $gamePath = (Resolve-Path -LiteralPath $GameExe -ErrorAction Stop).ProviderPath } catch { }
if (-not $gamePath) { Stop-Install -Text ('Path not found: ' + $GameExe) }

$gameExePath = $null
$gameRoot    = $null

if (Test-DirHere $gamePath) {
    $gameRoot = $gamePath
    $dlls = @()
    foreach ($n in $script:ReShadeDllNames) {
        $p = Join-Safe $gameRoot $n
        if (Test-FileHere $p) { $dlls += $p }
    }
    if ($dlls.Count -eq 0) {
        Stop-Install -Text ('No ReShade DLL beside ' + $gameRoot + '.') `
            -Detail ('Looked for: ' + ($script:ReShadeDllNames -join ', ')) `
            -Manual 'Install ReShade (Add-on support build, 6.8 or newer) into the game first, then re-run this installer.' `
            -Code $script:ExitNoReShade
    }
    $exes = @(Get-ChildItem -LiteralPath $gameRoot -File -Filter '*.exe' -ErrorAction SilentlyContinue |
              Where-Object { $_.Name -ine $script:HostExeName })
    if ($exes.Count -eq 0) {
        Stop-Install -Text ('No .exe found in ' + $gameRoot + '.')
    }
    if ($exes.Count -gt 1) {
        $list = ($exes | ForEach-Object { $_.Name }) -join "`n"
        Stop-Install -Text 'Several executables sit beside that ReShade DLL; say which one is the game.' -Detail $list `
            -Manual ('Re-run with the exe, e.g.: Install-OptimizerFPS.ps1 "' + (Join-Safe $gameRoot $exes[0].Name) + '"')
    }
    $gameExePath = $exes[0].FullName
}
elseif (Test-FileHere $gamePath) {
    $gameExePath = $gamePath
    $gameRoot    = Split-Path -Parent $gamePath
}
else {
    Stop-Install -Text ('Not a file and not a folder: ' + $gamePath)
}

$pe = Get-PeInfo $gameExePath
if ($null -eq $pe) { Stop-Install -Text ('Not a readable executable: ' + $gameExePath) }
if ($pe.Bits -ne 32 -and $pe.Bits -ne 64) {
    Stop-Install -Text ('Unsupported executable architecture: ' + $pe.Arch)
}
$arch = $pe.Arch
$script:Summary.Arch    = $arch
$script:Summary.GameExe = $gameExePath

Report -Status 'Info' -Text ('Game: ' + [IO.Path]::GetFileName($gameExePath) + ' (' + $arch + ')') -Detail $gameRoot

# --- 2. Locate ReShade -----------------------------------------------------------------

Write-Section 'ReShade'

function Find-ReShadeDll
{
    param([string] $Dir, [int] $Bits)
    $best = $null
    foreach ($n in $script:ReShadeDllNames) {
        $p = Join-Safe $Dir $n
        if (-not (Test-FileHere $p)) { continue }
        $info = Get-PeInfo $p
        if ($null -eq $info -or $info.Bits -ne $Bits) { continue }
        if (-not (Test-IsReShade $p)) { continue }
        if ((Test-ReShadeHasAddons $p) -eq $true) { return $p }
        if ($null -eq $best) { $best = $p }
    }
    return $best
}

$targetDir = $null
$remoteDir = $null
$reshadeDll = $null
$remoteReshadeDll = $null

if ($arch -eq 'x64') {
    $reshadeDll = Find-ReShadeDll -Dir $gameRoot -Bits 64
    if (-not $reshadeDll) {
        Stop-Install -Text 'No 64-bit ReShade DLL beside the game.' `
            -Detail ('Looked in ' + $gameRoot + ' for: ' + ($script:ReShadeDllNames -join ', ')) `
            -Manual 'Install ReShade 6.8+ (Add-on support build) into the game, then re-run this installer.' `
            -Code $script:ExitNoReShade
    }
    $targetDir = Split-Path -Parent $reshadeDll
}
else {
    $host64 = Join-Safe $gameRoot 'host64'
    $hostExe = Join-Safe $host64 $script:HostExeName
    if (-not (Test-FileHere $hostExe)) {
        Stop-Install -Text 'This is a 32-bit game and DLSS5-Feeder''s 64-bit host is not installed.' `
            -Detail ('Expected: ' + $hostExe) `
            -Manual ("A 32-bit process cannot load the 64-bit NGX runtime, so DLSS 5 Neural Rendering runs in`n" +
                     "DLSS5-Feeder's host64 helper. Run Install-DLSS5Feeder.ps1 for this game first, then re-run`n" +
                     'this installer.') `
            -Code $script:ExitNoHost64
    }
    $reshadeDll = Find-ReShadeDll -Dir $host64 -Bits 64
    if (-not $reshadeDll) {
        Stop-Install -Text 'No 64-bit ReShade DLL in host64.' -Detail $host64 `
            -Manual 'Re-run the DLSS5-Feeder installer: host64 needs its own 64-bit ReShade (Add-on build).' `
            -Code $script:ExitNoReShade
    }
    $targetDir = $host64

    $remoteReshadeDll = Find-ReShadeDll -Dir $gameRoot -Bits 32
    if (-not $remoteReshadeDll) {
        $binDir = Join-Safe $gameRoot 'bin'
        if (Test-DirHere $binDir) { $remoteReshadeDll = Find-ReShadeDll -Dir $binDir -Bits 32 }
    }
    if (-not $remoteReshadeDll) {
        Stop-Install -Text 'No 32-bit ReShade DLL beside the game.' -Detail $gameRoot `
            -Manual 'The remote tab loads into the game''s own 32-bit ReShade. Install it first.' `
            -Code $script:ExitNoReShade
    }
    $remoteDir = Split-Path -Parent $remoteReshadeDll

    $feed32 = Join-Safe $remoteDir 'dlss5-feed.addon32'
    if (-not (Test-FileHere $feed32)) {
        Report -Status 'Warn' -Text 'dlss5-feed.addon32 is not beside the 32-bit ReShade DLL.' `
            -Detail 'Without the Feeder add-on the game never hands its frames to host64, so nothing will be warped.'
    }
}

$reshadeVersion = Get-FileVersionSafe $reshadeDll
$versionOk = Test-ReShadeVersionOk $reshadeVersion
$hasAddons = Test-ReShadeHasAddons $reshadeDll

if ($hasAddons -eq $false) {
    Stop-Install -Text ('That ReShade build has no add-on support: ' + [IO.Path]::GetFileName($reshadeDll)) `
        -Detail 'It does not export ReShadeRegisterAddon, so no add-on can ever load into it.' `
        -Manual 'Re-run ReShade''s setup and pick the build "with full add-on support".' `
        -Code $script:ExitReShadeOld
}
if ($versionOk -eq $false) {
    Stop-Install -Text ('ReShade ' + $reshadeVersion + ' is too old (need ' + $script:ReShadeMinVersion + '+).') `
        -Detail $reshadeDll `
        -Manual 'Update ReShade to 6.8 or newer (Add-on support build).' `
        -Code $script:ExitReShadeOld
}

$vText = $reshadeVersion
if (-not $vText) { $vText = 'unknown version' }
Report -Status 'Ok' -Text ('ReShade ' + $vText + ' with add-on support: ' + [IO.Path]::GetFileName($reshadeDll)) -Detail $targetDir

$iniPath = Join-Safe $targetDir 'ReShade.ini'
$iniText = Read-TextSafe $iniPath

# [ADDON] AddonPath moves where add-ons are loaded from; the ini itself stays put.
$addonDir = $targetDir
$addonPathKey = Get-IniKey $iniText 'ADDON' 'AddonPath'
if ($addonPathKey) {
    $first = ($addonPathKey -split ',')[0].Trim()
    if ($first) {
        $resolved = $first
        if (-not [IO.Path]::IsPathRooted($resolved)) { $resolved = Join-Safe $targetDir $resolved }
        try { $resolved = [IO.Path]::GetFullPath($resolved) } catch { }
        if (Test-DirHere $resolved) {
            $addonDir = $resolved
            Report -Status 'Info' -Text 'ReShade.ini sets [ADDON] AddonPath; the add-on goes there.' -Detail $addonDir
        }
        else {
            Report -Status 'Warn' -Text ('[ADDON] AddonPath points at a folder that does not exist: ' + $resolved) `
                   -Detail 'Installing beside the ReShade DLL instead.'
        }
    }
}

$script:Summary.TargetDir = $targetDir
$script:Summary.AddonDir  = $addonDir
$script:Summary.RemoteDir = $remoteDir
$iniPaths = @($iniPath)
if ($remoteDir) { $iniPaths += (Join-Safe $remoteDir 'ReShade.ini') }
$script:Summary.IniPaths = $iniPaths

# --- 3. The neural-rendering consumer (warn only) --------------------------------------

Write-Section 'Neural rendering'

$consumerFound = @()
foreach ($d in @($addonDir, $targetDir, $gameRoot, $remoteDir)) {
    if (-not (Test-DirHere $d)) { continue }
    foreach ($pat in @('renodx-dlss5*.addon64', 'dlss5-feed*.addon64', 'dlss5-feed*.addon32')) {
        foreach ($h in @(Get-ChildItem -LiteralPath $d -File -Filter $pat -ErrorAction SilentlyContinue)) {
            $consumerFound += $h.FullName
        }
    }
    $opti = Join-Safe $d 'OptiScaler.ini'
    if (Test-FileHere $opti) {
        $t = Read-TextSafe $opti
        if ($t -and $t -match '(?im)^\s*\[DlssNr\]') { $consumerFound += $opti }
    }
}
$consumerFound = @($consumerFound | Sort-Object -Unique)

if ($consumerFound.Count -gt 0) {
    Report -Status 'Ok' -Text 'A DLSS 5 Neural Rendering consumer is present.' -Detail (($consumerFound | ForEach-Object { [IO.Path]::GetFileName($_) }) -join "`n")
}
else {
    Report -Status 'Warn' -Text 'No neural-rendering consumer found.' `
        -Detail ("Optimizer FPS only compresses what someone else feeds to feature 18. Without renodx-dlss5,`n" +
                 'DLSS5-Feeder or an OptiScaler fork with [DlssNr], nothing will call it.') `
        -Manual 'Install a neural-rendering consumer for this game (see docs\INSTALL.md).'
}

$nrRuntime = $null
foreach ($d in @($gameRoot, $targetDir, $addonDir)) {
    if (-not (Test-DirHere $d)) { continue }
    $p = Join-Safe $d 'nvngx_dlssnr.dll'
    if (Test-FileHere $p) { $nrRuntime = $p; break }
}
if ($nrRuntime) {
    $nrv = Get-FileVersionSafe $nrRuntime
    if (-not $nrv) { $nrv = 'unknown version' }
    Report -Status 'Ok' -Text ('nvngx_dlssnr.dll ' + $nrv) -Detail $nrRuntime
}
else {
    Report -Status 'Warn' -Text 'nvngx_dlssnr.dll was not found in the game or in host64.' `
        -Detail 'The driver may still supply it; if the game logs no feature 18, put the 310.8 runtime beside the game.'
}

# --- 4. Is anything running? -----------------------------------------------------------

$runningNames = @([IO.Path]::GetFileNameWithoutExtension($gameExePath))
if ($arch -eq 'x86') { $runningNames += [IO.Path]::GetFileNameWithoutExtension($script:HostExeName) }
foreach ($n in $runningNames) {
    $proc = @(Get-Process -Name $n -ErrorAction SilentlyContinue)
    if ($proc.Count -gt 0) {
        Report -Status 'Fail' -Text ($n + '.exe is running. Close it first; its files are locked.')
        Exit-Installer $script:ExitGameRunning
    }
}

Enter-InstallLock -Folder $targetDir

$stateDir    = Join-Safe $targetDir $script:StateFolderName
$latestPath  = Join-Safe $stateDir 'latest-receipt.json'

# ---------------------------------------------------------------------------------------
# Uninstall
# ---------------------------------------------------------------------------------------

if ($Mode -eq 'Uninstall') {
    Write-Section 'Uninstall'

    $receipt = $null
    if (Test-FileHere $latestPath) {
        try { $receipt = (Read-TextSafe $latestPath) | ConvertFrom-Json } catch { $receipt = $null }
    }

    $removed = 0
    $kept    = 0
    $restored = 0

    if ($receipt) {
        foreach ($f in @($receipt.Files)) {
            $p = [string]$f.Path
            if (-not (Test-FileHere $p)) { continue }
            # Only a file that is still byte-for-byte the one this installer wrote may go --
            # an adopted file is no different there, and one the user has changed since is
            # theirs now. Restoring a backed-up original therefore happens only over the gap
            # our own deletion just left, never on top of a file we decided to keep.
            $now = Get-Sha256 $p
            $deleted = $false
            if ([string]::Equals($now, [string]$f.InstalledHash, [StringComparison]::OrdinalIgnoreCase)) {
                try { Remove-Item -LiteralPath $p -Force; $removed++; $deleted = $true }
                catch { Report -Status 'Warn' -Text ('Could not remove ' + $p) -Detail $_.Exception.Message }
            }
            else {
                $kept++
                Report -Status 'Warn' -Text ('Changed since the install, kept: ' + (Get-RelativePathCompat $targetDir $p))
            }
            $hadOriginal = $false
            if ($f.PSObject.Properties['HadOriginal']) { $hadOriginal = [bool]$f.HadOriginal }
            if ($deleted -and -not (Test-FileHere $p) -and $hadOriginal -and
                $f.PSObject.Properties['BackupPath'] -and (Test-FileHere ([string]$f.BackupPath))) {
                try { Copy-FileAtomic -Source ([string]$f.BackupPath) -Destination $p; $restored++ }
                catch { Report -Status 'Warn' -Text ('Could not restore the original of ' + $p) -Detail $_.Exception.Message }
            }
        }
    }
    else {
        # No receipt: remove only what is unmistakably ours.
        $candidates = @()
        foreach ($d in @($addonDir)) {
            foreach ($n in @($script:AddonFileName, $script:ForwarderFileName,
                             $script:LegacyAddonFileName, $script:LegacyForwarderFileName)) {
                $p = Join-Safe $d $n
                if (Test-FileHere $p) { $candidates += @{ Path = $p; Kind = (Get-PayloadKind $n) } }
            }
            foreach ($sn in @($script:ShaderFolderName, $script:LegacyShaderFolderName)) {
                $sd = Join-Safe $d $sn
                if (-not (Test-DirHere $sd)) { continue }
                foreach ($h in @(Get-ChildItem -LiteralPath $sd -File -Filter '*.dxbc' -ErrorAction SilentlyContinue)) {
                    $candidates += @{ Path = $h.FullName; Kind = 'dxbc' }
                }
            }
        }
        if ($remoteDir) {
            foreach ($n in @($script:RemoteFileName, $script:LegacyRemoteFileName)) {
                $p = Join-Safe $remoteDir $n
                if (Test-FileHere $p) { $candidates += @{ Path = $p; Kind = 'addon32' } }
            }
        }
        if ($candidates.Count -eq 0) {
            Report -Status 'Fail' -Text 'Nothing to uninstall: no receipt and no Optimizer FPS files found.' -Detail $targetDir
            Exit-Installer $script:ExitNothingToDo
        }
        Report -Status 'Warn' -Text 'No receipt found; removing the files that are recognisably ours.'
        foreach ($c in $candidates) {
            if (-not (Test-IsOurFile $c.Path $c.Kind)) { $kept++; continue }
            try { Remove-Item -LiteralPath $c.Path -Force; $removed++ } catch { $kept++ }
        }
    }

    # The empty shader folder goes too -- under either name.
    foreach ($sn in @($script:ShaderFolderName, $script:LegacyShaderFolderName)) {
        $sd = Join-Safe $addonDir $sn
        if (-not (Test-DirHere $sd)) { continue }
        $left = @(Get-ChildItem -LiteralPath $sd -Force -ErrorAction SilentlyContinue)
        if ($left.Count -eq 0) { try { Remove-Item -LiteralPath $sd -Force } catch { } }
    }

    foreach ($mn in @($script:SessionMarker, $script:LegacySessionMarker)) {
        $marker = Join-Safe $addonDir $mn
        if (Test-FileHere $marker) { try { Remove-Item -LiteralPath $marker -Force } catch { } }
    }

    # Ini: only the keys this installer wrote. DisabledAddons keeps the migrated name --
    # reverting it would resurrect a name no build answers to any more.
    if ($receipt -and -not $NoIni -and $receipt.PSObject.Properties['IniKeysWritten']) {
        $byPath = @{}
        foreach ($k in @($receipt.IniKeysWritten)) {
            $kp = [string]$k.Path
            if (-not $byPath.ContainsKey($kp)) { $byPath[$kp] = New-Object System.Collections.ArrayList }
            $null = $byPath[$kp].Add($k)
        }
        foreach ($kp in $byPath.Keys) {
            $t = Read-TextSafe $kp
            if ($null -eq $t) { continue }
            $sections = @()
            foreach ($k in $byPath[$kp]) {
                $t = Remove-IniKey -Text $t -Section ([string]$k.Section) -Key ([string]$k.Key)
                $sections += [string]$k.Section
            }
            foreach ($s in ($sections | Sort-Object -Unique)) { $t = Remove-EmptyIniSection -Text $t -Section $s }
            Write-TextAtomic -Content $t -Path $kp
            Report -Status 'Done' -Text ('Removed the keys this installer wrote from ' + (Get-RelativePathCompat $targetDir $kp))
        }
    }

    # Whatever this installer added to Defender's exclusion list goes with it.
    if ($receipt -and $receipt.PSObject.Properties['DefenderExclusions']) {
        $ex = @($receipt.DefenderExclusions)
        if ($ex.Count -gt 0) { Remove-DefenderExclusion -Paths $ex }
    }

    if (Test-FileHere $latestPath) { try { Remove-Item -LiteralPath $latestPath -Force } catch { } }

    Report -Status 'Done' -Text ('Uninstalled: ' + $removed + ' file(s) removed, ' + $restored + ' original(s) restored, ' + $kept + ' left alone.') `
           -Detail ('The backup folders under ' + $stateDir + ' are kept.')
    Write-Section 'Summary'
    Write-Line ('  ' + $script:CountDone + ' done, ' + $script:CountWarn + ' warning(s), ' + $script:CountFail + ' failure(s).')
    if ($script:CountFail -gt 0) { Exit-Installer $script:ExitFail }
    Exit-Installer $script:ExitOk
}

# ---------------------------------------------------------------------------------------
# 5. Payload
# ---------------------------------------------------------------------------------------

Write-Section 'Payload'

if (-not (Test-DirHere $Payload)) {
    Report -Status 'Fail' -Text ('Payload folder not found: ' + $Payload) `
        -Manual 'Run this script from the unpacked release folder, or pass -Payload <dir>.'
    Exit-Installer $script:ExitPayloadBad
}

$manifestPath = Join-Safe $Payload 'files.sha256'
if (-not (Test-FileHere $manifestPath)) {
    Report -Status 'Fail' -Text ('payload\files.sha256 is missing: ' + $manifestPath)
    Exit-Installer $script:ExitPayloadBad
}

$payloadVersion = 'unknown'
$versionFile = Join-Safe $Payload 'VERSION.txt'
if (Test-FileHere $versionFile) {
    $vt = Read-TextSafe $versionFile
    if ($vt) { $payloadVersion = $vt.Trim() }
}
$script:Summary.Version = $payloadVersion

$manifest = New-Object System.Collections.ArrayList
foreach ($line in ((Read-TextSafe $manifestPath) -split "`r?`n")) {
    $t = $line.Trim()
    if (-not $t -or $t.StartsWith('#')) { continue }
    $m = [regex]::Match($t, '^([0-9a-fA-F]{64})\s+(.+)$')
    if (-not $m.Success) {
        Report -Status 'Fail' -Text ('Malformed line in files.sha256: ' + $t)
        Exit-Installer $script:ExitPayloadBad
    }
    $null = $manifest.Add(@{ Hash = $m.Groups[1].Value.ToUpperInvariant(); Rel = $m.Groups[2].Value.Trim() })
}
if ($manifest.Count -eq 0) {
    Report -Status 'Fail' -Text 'files.sha256 lists nothing.'
    Exit-Installer $script:ExitPayloadBad
}

$bad = @()
foreach ($e in $manifest) {
    $src = Join-Safe $Payload ($e.Rel -replace '/', '\')
    if (-not (Test-FileHere $src)) { $bad += ($e.Rel + ' (missing)'); continue }
    $got = Get-Sha256 $src
    if (-not [string]::Equals($got, $e.Hash, [StringComparison]::OrdinalIgnoreCase)) { $bad += ($e.Rel + ' (hash mismatch)') }
}
if ($bad.Count -gt 0) {
    $detail = ($bad | Select-Object -First 12) -join "`n"
    Report -Status 'Fail' -Text ('The payload is corrupt: ' + $bad.Count + ' file(s) do not match files.sha256.') -Detail $detail `
        -Manual 'Unpack the release zip again (Windows sometimes truncates files unpacked from a blocked archive).'
    Exit-Installer $script:ExitPayloadBad
}

Report -Status 'Ok' -Text ('Payload ' + $payloadVersion + ': ' + $manifest.Count + ' file(s) verified.') -Detail $Payload

# Payload entry -> destination.
$plan = New-Object System.Collections.ArrayList
foreach ($e in $manifest) {
    $rel = $e.Rel.Replace('\', '/')
    $dest = $null
    if ($rel -match '(?i)^x64/(.+)$') {
        $dest = Join-Safe $addonDir ($Matches[1] -replace '/', '\')
    }
    elseif ($rel -match '(?i)^x86/(.+)$') {
        if ($arch -ne 'x86') { continue }
        $dest = Join-Safe $remoteDir ($Matches[1] -replace '/', '\')
    }
    else { continue }   # VERSION.txt, files.sha256 and anything else stay in the payload
    $null = $plan.Add(@{ Source = (Join-Safe $Payload ($e.Rel -replace '/', '\')); Dest = $dest; Hash = $e.Hash; Rel = $rel; Kind = (Get-PayloadKind $rel) })
}

if ($plan.Count -eq 0) {
    Report -Status 'Fail' -Text 'The payload has nothing to install for this architecture.'
    Exit-Installer $script:ExitPayloadBad
}

$dxbcCount = @($plan | Where-Object { $_.Kind -eq 'dxbc' }).Count
Report -Status 'Info' -Text ('' + $plan.Count + ' file(s) to install, of which ' + $dxbcCount + ' compiled shader(s).')

# ---------------------------------------------------------------------------------------
# 6. Adoption: what is already there?
# ---------------------------------------------------------------------------------------

Write-Section 'Existing files'

$foreign = @()
foreach ($p in $plan) {
    $p['State'] = 'new'
    if (-not (Test-FileHere $p.Dest)) { continue }
    $now = Get-Sha256 $p.Dest
    if ([string]::Equals($now, $p.Hash, [StringComparison]::OrdinalIgnoreCase)) { $p['State'] = 'uptodate'; continue }
    if (Test-IsOurFile $p.Dest $p.Kind) { $p['State'] = 'adopt'; continue }
    $p['State'] = 'foreign'
    $foreign += $p.Dest
}

if ($foreign.Count -gt 0 -and -not $Force) {
    Stop-Install -Text ('Files with our names are already there and are NOT ours: ' + $foreign.Count) `
        -Detail (($foreign | Select-Object -First 8) -join "`n") `
        -Manual 'Move them aside yourself, or re-run with -Force (the installer then backs them up as originals).'
}
if ($foreign.Count -gt 0) {
    Report -Status 'Warn' -Text ('-Force: ' + $foreign.Count + ' foreign file(s) will be backed up and replaced.')
}

$adopted = @($plan | Where-Object { $_.State -eq 'adopt' })
$upToDate = @($plan | Where-Object { $_.State -eq 'uptodate' })
if ($adopted.Count -gt 0) {
    Report -Status 'Info' -Text ('Adopting ' + $adopted.Count + ' file(s) from an earlier (manual) install.')
}
if ($upToDate.Count -gt 0) {
    Report -Status 'Ok' -Text ('' + $upToDate.Count + ' file(s) already up to date.')
}

# Shaders that this release no longer has must go, or the add-on would keep loading them.
$staleShaders = @()
$shaderDir = Join-Safe $addonDir $script:ShaderFolderName
if (Test-DirHere $shaderDir) {
    $wanted = @{}
    foreach ($p in $plan) { if ($p.Kind -eq 'dxbc') { $wanted[[IO.Path]::GetFileName($p.Dest).ToLowerInvariant()] = $true } }
    foreach ($h in @(Get-ChildItem -LiteralPath $shaderDir -File -Filter '*.dxbc' -ErrorAction SilentlyContinue)) {
        if (-not $wanted.ContainsKey($h.Name.ToLowerInvariant())) { $staleShaders += $h.FullName }
    }
}
if ($staleShaders.Count -gt 0) {
    Report -Status 'Info' -Text ('' + $staleShaders.Count + ' shader(s) from an older release will be deleted.') `
           -Detail (($staleShaders | ForEach-Object { [IO.Path]::GetFileName($_) }) -join "`n")
}

# ---------------------------------------------------------------------------------------
# 7. Backup, install, receipt
# ---------------------------------------------------------------------------------------

Write-Section 'Install'

$stamp     = (Get-Date).ToString('yyyyMMdd-HHmmss')
$backupDir = Join-Safe $stateDir ('backup-' + $stamp)
New-DirSafe $backupDir
$script:Summary.BackupDir = $backupDir

foreach ($ip in $iniPaths) {
    if (-not (Test-FileHere $ip)) { continue }
    $tag = 'ReShade.ini.original'
    if ($ip -ne $iniPath) { $tag = 'ReShade.ini.remote.original' }
    Copy-FileAtomic -Source $ip -Destination (Join-Safe $backupDir $tag)
}

$receiptFiles = New-Object System.Collections.ArrayList
$installed = 0
$defenderAsked = $false

foreach ($p in $plan) {
    $rel = Get-RelativePathCompat $targetDir $p.Dest
    if ($p.State -eq 'uptodate') {
        $null = $receiptFiles.Add([ordered]@{
            Path = $p.Dest; InstalledHash = $p.Hash; HadOriginal = $false; Adopted = $false; BackupPath = $null
        })
        continue
    }

    $hadOriginal = $false
    $backupPath  = $null
    if ($p.State -eq 'foreign') {
        # $rel is an absolute path when the file lives outside TargetDir (the x86 remote
        # tab); strip the drive so it still lands inside the backup folder.
        $safeRel = $rel -replace '^[A-Za-z]:\\', '' -replace '^\\\\', ''
        $backupPath = Join-Safe $backupDir ('originals\' + $safeRel)
        Copy-FileAtomic -Source $p.Dest -Destination $backupPath
        $hadOriginal = $true
    }

    try { Copy-FileAtomic -Source $p.Source -Destination $p.Dest -ExpectedSha256 $p.Hash }
    catch {
        if (-not (Test-FileHere $p.Dest) -and -not $defenderAsked) {
            $defenderAsked = $true
            $det = Get-DefenderDetection $p.Dest
            if ($det) { $null = Request-DefenderExclusion -Paths @($p.Dest) -File ([IO.Path]::GetFileName($p.Dest)) }
        }
        Stop-Install -Text ('Could not install ' + $rel) -Detail $_.Exception.Message
    }

    $installed++
    $null = $receiptFiles.Add([ordered]@{
        Path        = $p.Dest
        InstalledHash = $p.Hash
        HadOriginal = $hadOriginal
        Adopted     = ($p.State -eq 'adopt')
        BackupPath  = $backupPath
    })
}

# The folder is ours, but a file in it that this release does not ship is still the user's copy:
# it goes to the backup folder first, and the receipt names it, so an uninstall can put it back.
$removedStaleShaders = New-Object System.Collections.ArrayList
foreach ($s in $staleShaders) {
    $leaf = [IO.Path]::GetFileName($s)
    $staleBackup = Join-Safe $backupDir (Join-Path 'stale-shaders' $leaf)
    try { Copy-FileAtomic -Source $s -Destination $staleBackup }
    catch {
        Report -Status 'Warn' -Text ('Could not back up the stale shader ' + $leaf + '; it was kept.') -Detail $_.Exception.Message
        continue
    }
    try {
        Remove-Item -LiteralPath $s -Force
        $null = $removedStaleShaders.Add([ordered]@{ Path = $s; BackupPath = $staleBackup })
    }
    catch { Report -Status 'Warn' -Text ('Could not delete the stale shader ' + $leaf) }
}

$legacyRemoved = @(Remove-LegacyInstall -AddonDir $addonDir -RemoteDir $remoteDir -BackupDir $backupDir -TargetDir $targetDir)
if ($legacyRemoved.Count -gt 0) {
    Report -Status 'Done' -Text ('' + $legacyRemoved.Count + ' file(s) from a pre-26.26 release removed (backed up first).') `
           -Detail (($legacyRemoved | ForEach-Object { [IO.Path]::GetFileName($_) }) -join "`n")
}

Report -Status 'Done' -Text ('' + $installed + ' file(s) installed into ' + $addonDir + '.')
if ($remoteDir) { Report -Status 'Done' -Text ('The remote tab went to ' + $remoteDir + '.') }

# A marker left behind means the previous session died right after its first warped frame,
# and the add-on would start in pass-through. A fresh install clears it.
$marker = Join-Safe $addonDir $script:SessionMarker
if (Test-FileHere $marker) {
    try { Remove-Item -LiteralPath $marker -Force; Report -Status 'Done' -Text ('Removed the stale crash-guard marker ' + $script:SessionMarker + '.') }
    catch { Report -Status 'Warn' -Text ('Could not remove ' + $marker) }
}

# ---------------------------------------------------------------------------------------
# 8. ReShade.ini
# ---------------------------------------------------------------------------------------

$iniKeysWritten = New-Object System.Collections.ArrayList
$migrated = New-Object System.Collections.ArrayList

if ($NoIni) {
    Write-Section 'Settings'
    Report -Status 'Skip' -Text '-NoIni: ReShade.ini was not touched.'
}
else {
    Write-Section 'Settings'

    $t = Read-TextSafe $iniPath
    if ($null -eq $t) { $t = '' }
    $wrote = 0
    foreach ($k in $script:DefaultIniKeys) {
        $existing = Get-IniKey $t 'PeripheralWarp' $k.Key
        if ($null -ne $existing) { continue }
        $t = Set-IniKey -Text $t -Section 'PeripheralWarp' -Key $k.Key -Value $k.Value
        $null = $iniKeysWritten.Add([ordered]@{ Path = $iniPath; Section = 'PeripheralWarp'; Key = $k.Key; Value = $k.Value })
        $wrote++
    }

    # DisabledAddons migration, on the host ini and (x86) on the game's own ini.
    $iniTexts = @{}
    $iniTexts[$iniPath] = $t
    if ($remoteDir) {
        $remoteIni = Join-Safe $remoteDir 'ReShade.ini'
        $rt = Read-TextSafe $remoteIni
        if ($null -ne $rt) { $iniTexts[$remoteIni] = $rt }
    }

    foreach ($ip in @($iniTexts.Keys)) {
        $text = $iniTexts[$ip]
        $cur = Get-IniKey $text 'ADDON' 'DisabledAddons'
        if ($null -eq $cur -or $cur.Trim() -eq '') { continue }
        $entries = @($cur -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
        $out = New-Object System.Collections.ArrayList
        $listChanged = $false
        foreach ($e in $entries) {
            $n = Convert-AddonNameToCurrent $e
            if ($n -ne $e) { $listChanged = $true; $null = $migrated.Add($e + ' -> ' + $n) }
            if (-not ($out -contains $n)) { $null = $out.Add($n) }
        }
        if ($listChanged) {
            $text = Set-IniKey -Text $text -Section 'ADDON' -Key 'DisabledAddons' -Value (($out.ToArray()) -join ',')
            $iniTexts[$ip] = $text
            Report -Status 'Done' -Text ('Carried the "disabled" state over to the new add-on name in ' + (Get-RelativePathCompat $gameRoot $ip) + '.')
        }
        if ($out -contains $script:AddonName -or $out -contains $script:RemoteAddonName) {
            Report -Status 'Warn' -Text 'The add-on is disabled in ReShade; enable it in Home -> Add-ons.' -Detail $ip
        }
    }

    foreach ($ip in @($iniTexts.Keys)) {
        $orig = Read-TextSafe $ip
        if ($orig -ne $iniTexts[$ip]) { Write-TextAtomic -Content $iniTexts[$ip] -Path $ip }
    }

    if ($wrote -gt 0) {
        Report -Status 'Done' -Text ('' + $wrote + ' key(s) written to [PeripheralWarp] in ReShade.ini.') `
               -Detail 'Mode=2 (peripheral), CenterX/CenterY=80, WorkX/WorkY=90 -- only keys that were not already set.'
    }
    else {
        Report -Status 'Ok' -Text '[PeripheralWarp] already has its settings; nothing was changed.'
    }
    if ($Mode -eq 'Update') {
        Report -Status 'Info' -Text 'Update mode: existing settings are never rewritten.'
    }
}

# An Update writes no ini key that is already there, so on its own its receipt would forget
# which keys the first install put in the file and the uninstall would leave them behind.
# The same goes for a foreign file that was backed up once and is ours from then on. Carry
# both forward from the receipt this run is about to replace.
$prevReceipt = $null
if (Test-FileHere $latestPath) {
    try { $prevReceipt = (Read-TextSafe $latestPath) | ConvertFrom-Json } catch { $prevReceipt = $null }
}
if ($prevReceipt) {
    if ($prevReceipt.PSObject.Properties['IniKeysWritten']) {
        $seenKeys = @{}
        foreach ($k in @($iniKeysWritten.ToArray())) {
            $seenKeys[(([string]$k.Path) + '|' + ([string]$k.Section) + '|' + ([string]$k.Key)).ToLowerInvariant()] = $true
        }
        foreach ($k in @($prevReceipt.IniKeysWritten)) {
            $id = (([string]$k.Path) + '|' + ([string]$k.Section) + '|' + ([string]$k.Key)).ToLowerInvariant()
            if ($seenKeys.ContainsKey($id)) { continue }
            $null = $iniKeysWritten.Add([ordered]@{ Path = [string]$k.Path; Section = [string]$k.Section; Key = [string]$k.Key; Value = [string]$k.Value })
            $seenKeys[$id] = $true
        }
    }
    if ($prevReceipt.PSObject.Properties['DefenderExclusions']) {
        foreach ($x in @($prevReceipt.DefenderExclusions)) {
            if (-not $x) { continue }
            $k = ([string]$x).ToLowerInvariant()
            if ($script:DefenderExcluded.ContainsKey($k)) { continue }
            $script:DefenderExcluded[$k] = $true
            $null = $script:DefenderExclusions.Add([string]$x)
        }
    }
    if ($prevReceipt.PSObject.Properties['Files']) {
        $prevFiles = @{}
        foreach ($f in @($prevReceipt.Files)) {
            if ($f.PSObject.Properties['HadOriginal'] -and [bool]$f.HadOriginal) { $prevFiles[([string]$f.Path).ToLowerInvariant()] = $f }
        }
        foreach ($e in @($receiptFiles.ToArray())) {
            $id = ([string]$e['Path']).ToLowerInvariant()
            if (-not $e['HadOriginal'] -and $prevFiles.ContainsKey($id)) {
                $e['HadOriginal'] = $true
                $e['BackupPath']  = [string]$prevFiles[$id].BackupPath
            }
        }
    }
}

$script:Summary.IniKeysWritten         = @($iniKeysWritten.ToArray())
$script:Summary.DisabledAddonsMigrated = @($migrated.ToArray())
$script:Summary.DefenderExclusions     = @($script:DefenderExclusions.ToArray())
$script:Summary.Files                  = @($receiptFiles.ToArray())
$script:Summary.RemovedLegacyFiles     = $legacyRemoved
$script:Summary.RemovedStaleShaders    = @($removedStaleShaders.ToArray())

# --- the receipt -----------------------------------------------------------------------

$receipt = [ordered]@{
    Schema                 = $script:ReceiptSchema
    Version                = $payloadVersion
    InstalledAt            = (Get-Date).ToString('o')
    Arch                   = $arch
    GameExe                = $gameExePath
    TargetDir              = $targetDir
    AddonDir               = $addonDir
    RemoteDir              = $remoteDir
    IniPaths               = $iniPaths
    Files                  = @($receiptFiles.ToArray())
    IniKeysWritten         = @($iniKeysWritten.ToArray())
    DisabledAddonsMigrated = @($migrated.ToArray())
    DefenderExclusions     = @($script:DefenderExclusions.ToArray())
    RemovedLegacyFiles     = $legacyRemoved
    RemovedStaleShaders    = @($removedStaleShaders.ToArray())
}

$receiptPath = Join-Safe $backupDir 'receipt.json'
Write-JsonAtomic -Value $receipt -Path $receiptPath
Write-JsonAtomic -Value $receipt -Path $latestPath
$script:Summary.ReceiptPath = $receiptPath
Report -Status 'Done' -Text 'Receipt written.' -Detail $receiptPath

# Mark of the Web: files unpacked from a downloaded zip are blocked and ReShade will not
# load them.
$unblocked = 0
foreach ($p in @($script:Changed.ToArray())) {
    try { if (Test-FileHere $p) { Unblock-File -LiteralPath $p -ErrorAction SilentlyContinue; $unblocked++ } } catch { }
}
if ($unblocked -gt 0) { Report -Status 'Ok' -Text ('Unblocked ' + $unblocked + ' file(s) (mark of the web).') }

# ---------------------------------------------------------------------------------------
# 9. Verify
# ---------------------------------------------------------------------------------------

$exitCode = $script:ExitOk

if ($NoVerify) {
    Report -Status 'Skip' -Text '-NoVerify: the install was not checked.'
}
elseif (-not (Test-FileHere $verifyScript)) {
    Report -Status 'Warn' -Text 'Verify-OptimizerFPS.ps1 is not beside this script; skipping the check.'
}
else {
    Write-Section 'Verify'
    $psExe = 'powershell.exe'
    try { $psExe = [Diagnostics.Process]::GetCurrentProcess().MainModule.FileName } catch { }
    $vArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $verifyScript, '-GameExe', $gameExePath)
    if ($Json) { $vArgs += '-Quiet' }
    & $psExe @vArgs
    $vCode = $LASTEXITCODE
    if ($null -eq $vCode) { $vCode = 0 }
    if ($vCode -eq $script:ExitNotVerified) {
        $exitCode = $script:ExitNotVerified
        Report -Status 'Info' -Text 'Installed, but the game has not run with it yet.' `
               -Detail 'Start the game once, then run Verify-OptimizerFPS.ps1 to see the runtime evidence.'
    }
    elseif ($vCode -ne 0) {
        Report -Status 'Warn' -Text ('Verify-OptimizerFPS.ps1 reported problems (exit ' + $vCode + ').')
    }
}

# ---------------------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------------------

Write-Section 'Summary'
Write-Line ('  ' + $script:CountDone + ' done, ' + $script:CountWarn + ' warning(s), ' + $script:CountFail + ' failure(s).')
Write-Line ''
Write-Chunk ('  Optimizer FPS for DLSS5 ' + $payloadVersion + ' is installed in ' + $addonDir) 'White'
Write-Chunk '  In the game: ReShade overlay (Home) -> Add-ons -> Optimizer FPS for DLSS5.' 'DarkGray'
if ($script:Manual.Count -gt 0) {
    Write-Line ''
    Write-Chunk '  Still to do by hand:' 'DarkYellow'
    foreach ($m in $script:Manual) { Write-Chunk ('    - ' + $m) 'DarkYellow' }
}

if ($script:CountFail -gt 0) { Exit-Installer $script:ExitFail }
Exit-Installer $exitCode

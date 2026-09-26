#Requires -Version 5.1
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
    param([string] $Version, [hashtable] $Context)
    if (-not $Version) { return $null }
    $m = [regex]::Match($Version, '^\s*(\d+)\.(\d+)')
    if (-not $m.Success) { return $null }
    $want = $Context.ReShadeMinVersion -split '\.'
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

function Find-ReShadeDll
{
    param([string] $Dir, [int] $Bits, [hashtable] $Context)
    $best = $null
    foreach ($n in $Context.ReShadeDllNames) {
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

function Resolve-InstallTarget
{
    param([string] $GameExe, [hashtable] $Context)
    & $Context.WriteSection 'Game'

    $gamePath = $null
    try { $gamePath = (Resolve-Path -LiteralPath $GameExe -ErrorAction Stop).ProviderPath } catch { }
    if (-not $gamePath) { & $Context.Stop -Text ('Path not found: ' + $GameExe) }

    $gameExePath = $null
    $gameRoot    = $null

    if (Test-DirHere $gamePath) {
        $gameRoot = $gamePath
        $dlls = @()
        foreach ($n in $Context.ReShadeDllNames) {
            $p = Join-Safe $gameRoot $n
            if (Test-FileHere $p) { $dlls += $p }
        }
        if ($dlls.Count -eq 0) {
            & $Context.Stop -Text ('No ReShade DLL beside ' + $gameRoot + '.') `
                -Detail ('Looked for: ' + ($Context.ReShadeDllNames -join ', ')) `
                -Manual 'Install ReShade (Add-on support build, 6.8 or newer) into the game first, then re-run this installer.' `
                -Code $Context.ExitNoReShade
        }
        $exes = @(Get-ChildItem -LiteralPath $gameRoot -File -Filter '*.exe' -ErrorAction SilentlyContinue |
                  Where-Object { $_.Name -ine $Context.HostExeName })
        if ($exes.Count -eq 0) {
            & $Context.Stop -Text ('No .exe found in ' + $gameRoot + '.')
        }
        if ($exes.Count -gt 1) {
            $list = ($exes | ForEach-Object { $_.Name }) -join "`n"
            & $Context.Stop -Text 'Several executables sit beside that ReShade DLL; say which one is the game.' -Detail $list `
                -Manual ('Re-run with the exe, e.g.: Install-OptimizerFPS.ps1 "' + (Join-Safe $gameRoot $exes[0].Name) + '"')
        }
        $gameExePath = $exes[0].FullName
    }
    elseif (Test-FileHere $gamePath) {
        $gameExePath = $gamePath
        $gameRoot    = Split-Path -Parent $gamePath
    }
    else {
        & $Context.Stop -Text ('Not a file and not a folder: ' + $gamePath)
    }

    $pe = Get-PeInfo $gameExePath
    if ($null -eq $pe) { & $Context.Stop -Text ('Not a readable executable: ' + $gameExePath) }
    if ($pe.Bits -ne 32 -and $pe.Bits -ne 64) {
        & $Context.Stop -Text ('Unsupported executable architecture: ' + $pe.Arch)
    }
    $arch = $pe.Arch
    $Context.Summary.Arch    = $arch
    $Context.Summary.GameExe = $gameExePath

    & $Context.Report -Status 'Info' -Text ('Game: ' + [IO.Path]::GetFileName($gameExePath) + ' (' + $arch + ')') -Detail $gameRoot

    # --- 2. Locate ReShade -----------------------------------------------------------------

    & $Context.WriteSection 'ReShade'


    $targetDir = $null
    $remoteDir = $null
    $reshadeDll = $null
    $remoteReshadeDll = $null

    if ($arch -eq 'x64') {
        $reshadeDll = Find-ReShadeDll -Dir $gameRoot -Bits 64 -Context $Context
        if (-not $reshadeDll) {
            & $Context.Stop -Text 'No 64-bit ReShade DLL beside the game.' `
                -Detail ('Looked in ' + $gameRoot + ' for: ' + ($Context.ReShadeDllNames -join ', ')) `
                -Manual 'Install ReShade 6.8+ (Add-on support build) into the game, then re-run this installer.' `
                -Code $Context.ExitNoReShade
        }
        $targetDir = Split-Path -Parent $reshadeDll
    }
    else {
        $host64 = Join-Safe $gameRoot 'host64'
        $hostExe = Join-Safe $host64 $Context.HostExeName
        $aioExe = Join-Safe $host64 $Context.AioHostExeName
        $aio = (-not (Test-FileHere $hostExe)) -and (Test-FileHere $aioExe)
        if ($aio) { $hostExe = $aioExe; $Context.HostExeName = $Context.AioHostExeName }  # DLSS5-Reshade-AIO
        if (-not (Test-FileHere $hostExe)) {
            & $Context.Stop -Text 'This is a 32-bit game and no 64-bit NR host (DLSS5-Feeder or DLSS5-Reshade-AIO) is installed.' `
                -Detail ('Expected: ' + $hostExe + ' or ' + $aioExe) `
                -Manual ("A 32-bit process cannot load the 64-bit NGX runtime, so DLSS 5 Neural Rendering runs in`n" +
                         "a 64-bit helper in host64: DLSS5-Feeder's host or DLSS5-Reshade-AIO's 32-bit wrapper. Install one`n" +
                         'of them for this game first (Install-DLSS5Feeder.ps1, or the AIO 32-bit zip), then re-run this installer.') `
                -Code $Context.ExitNoHost64
        }
        $reshadeDll = Find-ReShadeDll -Dir $host64 -Bits 64 -Context $Context
        if (-not $reshadeDll) {
            & $Context.Stop -Text 'No 64-bit ReShade DLL in host64.' -Detail $host64 `
                -Manual 'Re-run the DLSS5-Feeder installer: host64 needs its own 64-bit ReShade (Add-on build).' `
                -Code $Context.ExitNoReShade
        }
        $targetDir = $host64

        $remoteReshadeDll = Find-ReShadeDll -Dir $gameRoot -Bits 32 -Context $Context
        if (-not $remoteReshadeDll) {
            $binDir = Join-Safe $gameRoot 'bin'
            if (Test-DirHere $binDir) { $remoteReshadeDll = Find-ReShadeDll -Dir $binDir -Bits 32 -Context $Context }
        }
        if (-not $remoteReshadeDll) {
            & $Context.Stop -Text 'No 32-bit ReShade DLL beside the game.' -Detail $gameRoot `
                -Manual 'The remote tab loads into the game''s own 32-bit ReShade. Install it first.' `
                -Code $Context.ExitNoReShade
        }
        $remoteDir = Split-Path -Parent $remoteReshadeDll

        # Both helpers in host64: the 32-bit add-on beside the game says which one runs.
        if ((-not $aio) -and (Test-FileHere $aioExe) -and
            (Test-FileHere (Join-Safe $remoteDir 'standalone-dlssnr.addon32')) -and
            (-not (Test-FileHere (Join-Safe $remoteDir 'dlss5-feed.addon32')))) {
            $aio = $true
            $Context.HostExeName = $Context.AioHostExeName
        }
        $feedName = if ($aio) { 'standalone-dlssnr.addon32' } else { 'dlss5-feed.addon32' }
        $feed32 = Join-Safe $remoteDir $feedName
        if (-not (Test-FileHere $feed32)) {
            & $Context.Report -Status 'Warn' -Text ($feedName + ' is not beside the 32-bit ReShade DLL.') `
                -Detail 'Without the Feeder add-on the game never hands its frames to host64, so nothing will be warped.'
        }
    }

    $reshadeVersion = Get-FileVersionSafe $reshadeDll
    $versionOk = Test-ReShadeVersionOk $reshadeVersion -Context $Context
    $hasAddons = Test-ReShadeHasAddons $reshadeDll

    if ($hasAddons -eq $false) {
        & $Context.Stop -Text ('That ReShade build has no add-on support: ' + [IO.Path]::GetFileName($reshadeDll)) `
            -Detail 'It does not export ReShadeRegisterAddon, so no add-on can ever load into it.' `
            -Manual 'Re-run ReShade''s setup and pick the build "with full add-on support".' `
            -Code $Context.ExitReShadeOld
    }
    if ($versionOk -eq $false) {
        & $Context.Stop -Text ('ReShade ' + $reshadeVersion + ' is too old (need ' + $Context.ReShadeMinVersion + '+).') `
            -Detail $reshadeDll `
            -Manual 'Update ReShade to 6.8 or newer (Add-on support build).' `
            -Code $Context.ExitReShadeOld
    }

    $vText = $reshadeVersion
    if (-not $vText) { $vText = 'unknown version' }
    & $Context.Report -Status 'Ok' -Text ('ReShade ' + $vText + ' with add-on support: ' + [IO.Path]::GetFileName($reshadeDll)) -Detail $targetDir

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
                & $Context.Report -Status 'Info' -Text 'ReShade.ini sets [ADDON] AddonPath; the add-on goes there.' -Detail $addonDir
            }
            else {
                & $Context.Report -Status 'Warn' -Text ('[ADDON] AddonPath points at a folder that does not exist: ' + $resolved) `
                       -Detail 'Installing beside the ReShade DLL instead.'
            }
        }
    }

    $Context.Summary.TargetDir = $targetDir
    $Context.Summary.AddonDir  = $addonDir
    $Context.Summary.RemoteDir = $remoteDir
    $iniPaths = @($iniPath)
    if ($remoteDir) { $iniPaths += (Join-Safe $remoteDir 'ReShade.ini') }
    $Context.Summary.IniPaths = $iniPaths


    $Context.GameRoot = $gameRoot
    $Context.GameExePath = $gameExePath
    $Context.Arch = $arch
    $Context.TargetDir = $targetDir
    $Context.RemoteDir = $remoteDir
    $Context.AddonDir = $addonDir
    $Context.IniPath = $iniPath
    $Context.IniPaths = $iniPaths
}

function Test-NeuralConsumer
{
    param([string] $AddonDir, [string] $TargetDir, [string] $GameRoot, [string] $RemoteDir, [hashtable] $Context)
    & $Context.WriteSection 'Neural rendering'

    $consumerFound = @()
    foreach ($d in @($addonDir, $targetDir, $gameRoot, $remoteDir)) {
        if (-not (Test-DirHere $d)) { continue }
        foreach ($pat in @('renodx-dlss*.addon64', 'dlss5-feed*.addon64', 'dlss5-feed*.addon32', 'standalone-dlssnr.addon64', 'standalone-dlssnr.addon32')) {
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
        & $Context.Report -Status 'Ok' -Text 'A DLSS 5 Neural Rendering consumer is present.' -Detail (($consumerFound | ForEach-Object { [IO.Path]::GetFileName($_) }) -join "`n")
    }
    else {
        & $Context.Report -Status 'Warn' -Text 'No neural-rendering consumer found.' `
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
        & $Context.Report -Status 'Ok' -Text ('nvngx_dlssnr.dll ' + $nrv) -Detail $nrRuntime
    }
    else {
        & $Context.Report -Status 'Warn' -Text 'nvngx_dlssnr.dll was not found in the game or in host64.' `
            -Detail 'The driver may still supply it; if the game logs no feature 18, put the 310.8 runtime beside the game.'
    }
}

Export-ModuleMember -Function Test-NeuralConsumer, Resolve-InstallTarget, Get-PeInfo, Convert-RvaToOffset, Get-PeExportNames, Test-ReShadeHasAddons, Test-ReShadeVersionOk, Test-IsReShade, Find-ReShadeDll

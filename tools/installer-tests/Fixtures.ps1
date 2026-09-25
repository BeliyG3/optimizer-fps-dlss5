#Requires -Version 5.1
$cmd64 = Join-Path $env:WINDIR 'System32\cmd.exe'
$cmd32 = Join-Path $env:WINDIR 'SysWOW64\cmd.exe'

# A hash list of a folder, so "the uninstall put it back exactly" is a real assertion.
function Get-TreeSnapshot
{
    param([string] $Root, [string[]] $Exclude = @())
    $prefix = (Resolve-Path -LiteralPath $Root).ProviderPath
    if (-not $prefix.EndsWith('\')) { $prefix += '\' }
    $lines = New-Object System.Collections.ArrayList
    foreach ($f in @(Get-ChildItem -LiteralPath $Root -File -Recurse -Force -ErrorAction SilentlyContinue)) {
        $rel = $f.FullName.Substring($prefix.Length).Replace('\', '/')
        $skip = $false
        foreach ($x in $Exclude) { if ($rel -like $x) { $skip = $true; break } }
        if ($skip) { continue }
        $null = $lines.Add($rel.ToLowerInvariant() + '  ' + (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash)
    }
    return (@($lines.ToArray() | Sort-Object) -join "`n")
}

$MinimalIni = @'
[ADDON]
DisabledAddons=

[GENERAL]
EffectSearchPaths=.\

'@

function New-X64Fixture
{
    param([string] $Name, [string] $IniText, [string] $Root, [string] $ReShade64)
    $dir = Join-Path $Root $Name
    $null = New-Item -ItemType Directory -Path $dir -Force
    Copy-Item -LiteralPath $cmd64 -Destination (Join-Path $dir 'pwgame.exe') -Force
    Copy-Item -LiteralPath $ReShade64 -Destination (Join-Path $dir 'dxgi.dll') -Force
    $t = $IniText
    if (-not $t) { $t = $MinimalIni }
    [IO.File]::WriteAllText((Join-Path $dir 'ReShade.ini'), ($t -replace "`r`n", "`n" -replace "`n", "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $dir
}

function New-X86Fixture
{
    param([string] $Name, [string] $Root, [string] $ReShade64, [string] $ReShade32)
    $dir = Join-Path $Root $Name
    $null = New-Item -ItemType Directory -Path $dir -Force
    Copy-Item -LiteralPath $cmd32 -Destination (Join-Path $dir 'game.exe') -Force
    Copy-Item -LiteralPath $ReShade32 -Destination (Join-Path $dir 'dxgi.dll') -Force
    [IO.File]::WriteAllText((Join-Path $dir 'ReShade.ini'), ($MinimalIni -replace "`n", "`r`n"), (New-Object Text.UTF8Encoding($false)))
    # A Feeder install: the 32-bit side has the feeder add-on, the 64-bit host has ReShade.
    [IO.File]::WriteAllText((Join-Path $dir 'dlss5-feed.addon32'), 'stub', (New-Object Text.UTF8Encoding($false)))
    $h = Join-Path $dir 'host64'
    $null = New-Item -ItemType Directory -Path $h -Force
    Copy-Item -LiteralPath $cmd64 -Destination (Join-Path $h 'dlss5-feed-host64.exe') -Force
    Copy-Item -LiteralPath $ReShade64 -Destination (Join-Path $h 'dxgi.dll') -Force
    [IO.File]::WriteAllText((Join-Path $h 'ReShade.ini'), ($MinimalIni -replace "`n", "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $dir
}

function Get-PayloadShaderNames
{
    param([string] $Payload)
    $names = @()
    $mf = Join-Path $Payload 'files.sha256'
    foreach ($l in ([IO.File]::ReadAllText($mf) -split "`r?`n")) {
        $m = [regex]::Match($l.Trim(), '^[0-9a-f]{64}\s+x64/optimizer-fps-dlss5/(.+\.dxbc)$')
        if ($m.Success) { $names += $m.Groups[1].Value }
    }
    return $names
}

# A copy of the payload with VERSION.txt written by hand, or with no VERSION.txt at all.
# files.sha256 covers VERSION.txt too, so the manifest line has to be rewritten or dropped
# along with the file: otherwise the installer refuses the whole payload as corrupt and
# never gets as far as reading a version.
function New-PayloadWithVersion
{
    param([string] $Name, [byte[]] $Bytes, [switch] $Drop, [string] $Root, [string] $Payload)
    $dir = Join-Path $Root $Name
    Copy-Item -LiteralPath $Payload -Destination $dir -Recurse -Force
    $versionFile = Join-Path $dir 'VERSION.txt'
    $manifest = Join-Path $dir 'files.sha256'
    $lines = New-Object System.Collections.ArrayList
    foreach ($l in ([IO.File]::ReadAllText($manifest) -split "`r?`n")) {
        if ($l.Trim() -and $l -notmatch '(?i)\s+VERSION\.txt\s*$') { $null = $lines.Add($l) }
    }
    if ($Drop) {
        Remove-Item -LiteralPath $versionFile -Force
    }
    else {
        [IO.File]::WriteAllBytes($versionFile, $Bytes)
        $hash = (Get-FileHash -LiteralPath $versionFile -Algorithm SHA256).Hash.ToLowerInvariant()
        $null = $lines.Insert(0, ($hash + '  VERSION.txt'))
    }
    [IO.File]::WriteAllText($manifest, ((@($lines.ToArray()) -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))
    return $dir
}

# A wrong version usually differs from the right one by a character nobody can see, so the
# failure detail spells the string out code point by code point rather than just printing it.
function Format-VersionDetail
{
    param([string] $Text)
    $s = [string] $Text
    if (-not $s) { return '<empty>' }
    return ('<' + $s + '> [' + ((@($s.ToCharArray() | ForEach-Object { '{0:x4}' -f [int]$_ })) -join ' ') + ']')
}

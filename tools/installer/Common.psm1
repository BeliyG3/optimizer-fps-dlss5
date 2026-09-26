#Requires -Version 5.1
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
    param([string] $Source, [string] $Destination, [string] $ExpectedSha256, [hashtable] $Context)
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
    $null = $Context.Changed.Add($Destination)
}

function Write-TextAtomic
{
    param([string] $Content, [string] $Path, [hashtable] $Context,
          [Text.Encoding] $Encoding = (New-Object Text.UTF8Encoding($false)))
    $parent = Split-Path -Parent $Path
    New-DirSafe $parent
    $temporary = Join-Safe $parent ('.optimizerfps-write-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [IO.File]::WriteAllText($temporary, $Content, $Encoding)
        Move-Item -LiteralPath $temporary -Destination $Path -Force
    }
    finally {
        if (Test-Path -LiteralPath $temporary) { try { Remove-Item -LiteralPath $temporary -Force } catch { } }
    }
    $null = $Context.Changed.Add($Path)
}

function Write-JsonAtomic
{
    param($Value, [string] $Path, [hashtable] $Context)
    Write-TextAtomic -Content ($Value | ConvertTo-Json -Depth 32) -Path $Path -Context $Context
}

function Enter-InstallLock
{
    param([string] $Folder, [hashtable] $Context)
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
        & $Context.Stop -Text 'Another Optimizer FPS install is already running for this game folder.' `
                     -Detail $Folder
    }
    $Context.Lock = $mutex
}


function Test-InstallProcesses
{
    param([string] $GameExePath, [string] $TargetDir, [string] $Arch, [hashtable] $Context)
    # By full path: every 32-bit kit has its own dlss5-feed-host64.exe, and another game's host (or a
    # second copy of this game elsewhere) locks none of these files. A process whose path cannot be read
    # still counts - the safe side.
    $runningPaths = @($gameExePath)
    if ($arch -eq 'x86') {
        # Both 64-bit helpers: host64 may hold DLSS5-Feeder's host and DLSS5-Reshade-AIO's wrapper.
        $runningPaths += (Join-Safe $targetDir $Context.HostExeName)
        if ($Context.AioHostExeName -and $Context.AioHostExeName -ne $Context.HostExeName) {
            $runningPaths += (Join-Safe $targetDir $Context.AioHostExeName)
        }
    }
    foreach ($runningPath in $runningPaths) {
        $n = [IO.Path]::GetFileNameWithoutExtension($runningPath)
        $proc = @(Get-Process -Name $n -ErrorAction SilentlyContinue | Where-Object {
            $procPath = $null
            try { $procPath = $_.Path } catch { $procPath = $null }
            (-not $procPath) -or [string]::Equals([IO.Path]::GetFullPath($procPath), [IO.Path]::GetFullPath($runningPath),
                                                  [StringComparison]::OrdinalIgnoreCase)
        })
        if ($proc.Count -gt 0) {
            & $Context.Report -Status 'Fail' -Text ($n + '.exe is running. Close it first; its files are locked.')
            & $Context.Exit $Context.ExitGameRunning
        }
    }
}

Export-ModuleMember -Function Test-InstallProcesses, Join-Safe, Test-FileHere, Test-DirHere, New-DirSafe, Get-Sha256, Get-FileVersionSafe, Get-ProductNameSafe, Get-BinaryMarker, Read-TextSafe, Get-RelativePathCompat, Copy-FileAtomic, Write-TextAtomic, Write-JsonAtomic, Enter-InstallLock

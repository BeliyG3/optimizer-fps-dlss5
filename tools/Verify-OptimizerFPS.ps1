#Requires -Version 5.1
<#
.SYNOPSIS
    Checks an "Optimizer FPS for DLSS5" install: the files on disk, and what ReShade.log
    says the add-on actually did the last time the game ran.

.DESCRIPTION
    Copyright (c) 2026 Yuri Grib (BeliyG3). MIT licence (see LICENSE).

    Installer modules live beside this entry in installer/.
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
    [string] $CoreVersionTool,
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


foreach ($name in @('VerifyFiles', 'VerifyRuntime', 'Common', 'Pe', 'VerifyCore', 'Ini')) {
    $modulePath = Join-Path (Join-Path $PSScriptRoot 'installer') ($name + '.psm1')
    Import-Module -Name $modulePath -Force -ErrorAction Stop
}

$verifyContext = @{
    ReShadeDllNames = $script:ReShadeDllNames
    AddonName = $script:AddonName
    RemoteAddonName = $script:RemoteAddonName
    AddonFileName = $script:AddonFileName
    ForwarderFileName = $script:ForwarderFileName
    RemoteFileName = $script:RemoteFileName
    ShaderFolderName = $script:ShaderFolderName
    SessionMarker = $script:SessionMarker
    LegacyAddonFileName = $script:LegacyAddonFileName
    LegacyForwarderFileName = $script:LegacyForwarderFileName
    LegacyRemoteFileName = $script:LegacyRemoteFileName
    LegacyShaderFolderName = $script:LegacyShaderFolderName
    LegacySessionMarker = $script:LegacySessionMarker
    StateFolderName = $script:StateFolderName
    ReShadeMinVersion = $script:ReShadeMinVersion
    Result = $script:Result
    Report = { param($Status, $Text, $Detail) Report @PSBoundParameters }.GetNewClosure()
    WriteSection = { param($Title) Write-Section -Title $Title }.GetNewClosure()
}

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

$bits = Get-VerifyPeBits $gameExePath
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
    $reshadeDll = Find-VerifyReShadeDll -Context $verifyContext -Dir $gameRoot -Bits 64
    if ($reshadeDll) { $targetDir = Split-Path -Parent $reshadeDll }
}
else {
    $host64 = Join-Safe $gameRoot 'host64'
    $reshadeDll = Find-VerifyReShadeDll -Context $verifyContext -Dir $host64 -Bits 64
    if ($reshadeDll) { $targetDir = $host64 }
    $r32 = Find-VerifyReShadeDll -Context $verifyContext -Dir $gameRoot -Bits 32
    if (-not $r32) {
        $binDir = Join-Safe $gameRoot 'bin'
        if (Test-DirHere $binDir) { $r32 = Find-VerifyReShadeDll -Context $verifyContext -Dir $binDir -Bits 32 }
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

if (-not $Payload) { $Payload = Join-Path $PSScriptRoot 'payload' }
$verifyContext.GameRoot = $gameRoot
$verifyContext.TargetDir = $targetDir
$verifyContext.AddonDir = $addonDir
$verifyContext.RemoteDir = $remoteDir
$verifyContext.Arch = $arch
$verifyContext.ReShadeDll = $reshadeDll
$verifyContext.IniPath = $iniPath
$verifyContext.IniText = $iniText
$verifyContext.Payload = $Payload
$verifyContext.CoreVersionTool = $CoreVersionTool
Invoke-VerifyStatic -Context $verifyContext
Invoke-VerifyRuntime -Context $verifyContext
$missing = $verifyContext.Missing
$anyRuntime = $verifyContext.AnyRuntime
$rt = $script:Result.Runtime

# ---------------------------------------------------------------------------------------

Write-Section 'Verdict'

if ($missing -gt 0 -or $script:CountFail -gt 0) {
    Write-Chunk ('  ' + $script:CountFail + ' failure(s), ' + $script:CountWarn + ' warning(s).') 'Red'
    Exit-Verify $script:ExitProblems
}
if (-not $anyRuntime -or -not $rt['Loaded'] -or $rt['Core'].Verdict -ne 'loaded') {
    Write-Chunk '  Installed. Start the game once, then run Verify-OptimizerFPS.ps1 again.' 'Yellow'
    Exit-Verify $script:ExitNotVerified
}
Write-Chunk ('  Everything checks out (' + $script:CountWarn + ' warning(s)).') 'Green'
Exit-Verify $script:ExitOk

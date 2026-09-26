#Requires -Version 5.1
<#
.SYNOPSIS
    Installs "Optimizer FPS for DLSS5" (the ReShade add-on) into a game.

.DESCRIPTION
    Copyright (c) 2026 Yuri Grib (BeliyG3). MIT licence (see LICENSE).

    Puts optimizer-fps-dlss5.addon64, its NGX forwarder and its compiled shaders beside the
    ReShade DLL a game already has, writes a starting [OptimizerFPS] block into
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

Import-Module -Name (Join-Path $PSScriptRoot 'installer\Common.psm1') -Force -ErrorAction Stop
Import-Module -Name (Join-Path $PSScriptRoot 'installer\Pe.psm1') -Force -ErrorAction Stop
Import-Module -Name (Join-Path $PSScriptRoot 'installer\Ini.psm1') -Force -ErrorAction Stop
Import-Module -Name (Join-Path $PSScriptRoot 'installer\IniMigration.psm1') -Force -ErrorAction Stop
Import-Module -Name (Join-Path $PSScriptRoot 'installer\IniFile.psm1') -Force -ErrorAction Stop
Import-Module -Name (Join-Path $PSScriptRoot 'installer\InstallIni.psm1') -Force -ErrorAction Stop
Import-Module -Name (Join-Path $PSScriptRoot 'installer\Defender.psm1') -Force -ErrorAction Stop
Import-Module -Name (Join-Path $PSScriptRoot 'installer\InstallFiles.psm1') -Force -ErrorAction Stop

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
$script:AioHostExeName    = 'AIO DLSS5 32-bit Wrapper.exe'   # DLSS5-Reshade-AIO's host64 for 32-bit games
$script:ReceiptSchema     = 3

# A fresh host ini gets these defaults only when neither settings section exists.
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
    Core                   = $null
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

$script:Lock = $null
$script:DefenderExcluded = @{}
$script:DefenderExclusions = New-Object System.Collections.ArrayList
$script:InstallerContext = @{
    Changed = $script:Changed
    Lock = $null
    DefenderExcluded = $script:DefenderExcluded
    DefenderExclusions = $script:DefenderExclusions
    ReShadeMinVersion = $script:ReShadeMinVersion
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
    HostExeName = $script:HostExeName
    AioHostExeName = $script:AioHostExeName
    DefaultIniKeys = $script:DefaultIniKeys
    ReceiptSchema = $script:ReceiptSchema
    ExitNoReShade = $script:ExitNoReShade
    ExitReShadeOld = $script:ExitReShadeOld
    ExitNoHost64 = $script:ExitNoHost64
    ExitPayloadBad = $script:ExitPayloadBad
    ExitNothingToDo = $script:ExitNothingToDo
    ExitGameRunning = $script:ExitGameRunning
    Summary = $script:Summary
    AllowDefenderExclusion = [bool]$AllowDefenderExclusion
    NoIni = [bool]$NoIni
    Force = [bool]$Force
    Mode = $Mode
    Report = { param($Status, $Text, $Detail, $Manual) Report -Status $Status -Text $Text -Detail $Detail -Manual $Manual }
    Stop = { param($Text, $Detail, $Manual, $Code = 1) Stop-Install -Text $Text -Detail $Detail -Manual $Manual -Code $Code }
    WriteChunk = { param($Text, $Colour, $NoNewline) Write-Chunk -Text $Text -Colour $Colour -NoNewline:$NoNewline }
    WriteLine = { param($Text) Write-Line -Text $Text }
    WriteSection = { param($Title) Write-Section -Title $Title }
    Confirm = { param($Question) Confirm-Step -Question $Question }
    Exit = { param($Code) Exit-Installer -Code $Code }
}

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

    if ($script:InstallerContext.Lock) { try { $script:InstallerContext.Lock.ReleaseMutex() } catch { } ; try { $script:InstallerContext.Lock.Dispose() } catch { } ; $script:InstallerContext.Lock = $null }

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

# --- 1-2. Resolve game and ReShade ----------------------------------------------------
Resolve-InstallTarget -GameExe $GameExe -Context $script:InstallerContext
$gameRoot = $script:InstallerContext.GameRoot
$gameExePath = $script:InstallerContext.GameExePath
$arch = $script:InstallerContext.Arch
$targetDir = $script:InstallerContext.TargetDir
$remoteDir = $script:InstallerContext.RemoteDir
$addonDir = $script:InstallerContext.AddonDir
$iniPath = $script:InstallerContext.IniPath
$iniPaths = $script:InstallerContext.IniPaths

# --- 3. Neural-rendering consumer ------------------------------------------------------
Test-NeuralConsumer -AddonDir $addonDir -TargetDir $targetDir -GameRoot $gameRoot -RemoteDir $remoteDir -Context $script:InstallerContext

# --- 4. Process and install lock --------------------------------------------------------
Test-InstallProcesses -GameExePath $gameExePath -TargetDir $targetDir -Arch $arch -Context $script:InstallerContext
Enter-InstallLock -Folder $targetDir -Context $script:InstallerContext

$stateDir    = Join-Safe $targetDir $script:StateFolderName
$latestPath  = Join-Safe $stateDir 'latest-receipt.json'
$script:InstallerContext.LatestPath = $latestPath

# ---------------------------------------------------------------------------------------
# Uninstall
# ---------------------------------------------------------------------------------------

if ($Mode -eq 'Uninstall') {
    Invoke-InstallerUninstall -LatestPath $latestPath -TargetDir $targetDir -AddonDir $addonDir -RemoteDir $remoteDir -StateDir $stateDir -Context $script:InstallerContext
    Write-Section 'Summary'
    Write-Line ('  ' + $script:CountDone + ' done, ' + $script:CountWarn + ' warning(s), ' + $script:CountFail + ' failure(s).')
    if ($script:CountFail -gt 0) { Exit-Installer $script:ExitFail }
    Exit-Installer $script:ExitOk
}

# 5. Payload
$payloadState = Read-InstallPayload -Payload $Payload -AddonDir $addonDir -RemoteDir $remoteDir -Arch $arch -Context $script:InstallerContext
$plan = $payloadState.Plan
$payloadVersion = $payloadState.Version

# 6. Adoption
$adoption = Get-InstallAdoption -Plan $plan -AddonDir $addonDir -Context $script:InstallerContext
$staleShaders = $adoption.StaleShaders

# 7. Backup and install
$installedState = Install-FilePayload -Plan $plan -StaleShaders $staleShaders -StateDir $stateDir -TargetDir $targetDir -AddonDir $addonDir -RemoteDir $remoteDir -IniPath $iniPath -IniPaths $iniPaths -Context $script:InstallerContext
$backupDir = $installedState.BackupDir
$receiptFiles = $installedState.ReceiptFiles
$removedStaleShaders = $installedState.RemovedStaleShaders
$legacyRemoved = $installedState.LegacyRemoved

# 8. ReShade.ini
try {
    $iniState = Install-IniSettings -IniPath $iniPath -RemoteDir $remoteDir -GameRoot $gameRoot -Mode $Mode -LatestPath $latestPath -ReceiptFiles $receiptFiles -LegacyRemoved $legacyRemoved -RemovedStaleShaders $removedStaleShaders -Context $script:InstallerContext
}
catch {
    Stop-Install -Text 'Could not update ReShade.ini.' -Detail $_.Exception.Message
}
$iniKeysWritten = $iniState.IniKeysWritten
$migrated = $iniState.Migrated

# Receipt and mark-of-the-web
Write-InstallReceipt -Context $script:InstallerContext

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

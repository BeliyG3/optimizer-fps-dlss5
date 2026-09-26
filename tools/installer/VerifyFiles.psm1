#Requires -Version 5.1
foreach ($name in @('Common', 'Pe', 'Ini', 'VerifyCore')) {
    Import-Module -Name (Join-Path $PSScriptRoot ($name + '.psm1')) -Force -ErrorAction Stop
}

function Get-VerifyPeBits
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

function Find-VerifyReShadeDll
{
    param([string] $Dir, [int] $Bits, [hashtable] $Context)
    $best = $null
    foreach ($n in $Context.ReShadeDllNames) {
        $p = Join-Safe $Dir $n
        if (-not (Test-FileHere $p)) { continue }
        if ((Get-VerifyPeBits $p) -ne $Bits) { continue }
        if ((Test-ReShadeHasAddons $p) -eq $true) { return $p }
        if ($null -eq $best) {
            $pn = $null
            try { $pn = (Get-Item -LiteralPath $p).VersionInfo.ProductName } catch { }
            if ($pn -and $pn -match '(?i)reshade') { $best = $p }
        }
    }
    return $best
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

function Invoke-VerifyStatic
{
    param([hashtable] $Context)
    $targetDir = $Context.TargetDir
    $addonDir = $Context.AddonDir
    $remoteDir = $Context.RemoteDir
    $gameRoot = $Context.GameRoot
    $arch = $Context.Arch
    $reshadeDll = $Context.ReShadeDll
    $iniPath = $Context.IniPath
    $iniText = $Context.IniText
    $Payload = $Context.Payload
& $Context.WriteSection 'Files'

$stateDir   = Join-Safe $targetDir $Context.StateFolderName
$latestPath = Join-Safe $stateDir 'latest-receipt.json'
$receipt = $null
if (Test-FileHere $latestPath) {
    try { $receipt = (Read-TextSafe $latestPath) | ConvertFrom-Json } catch { $receipt = $null }
}
if ($receipt -and $receipt.PSObject.Properties['Version']) { $Context.Result.Version = [string]$receipt.Version }

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
    $vf = Join-Safe $Payload 'VERSION.txt'
    if ($manifest.ContainsKey('VERSION.txt') -and (Test-FileHere $vf) -and
        [string]::Equals((Get-Sha256 $vf), $manifest['VERSION.txt'], [StringComparison]::OrdinalIgnoreCase)) {
        $Context.Result.Version = (Read-TextSafe $vf).Trim()
    }
}
elseif ($receipt) { $manifestSource = 'receipt' }
if (-not $Context.Result.Version -and -not $manifestSource) {
    $versionCmake = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'cmake\Version.cmake'
    if (Test-FileHere $versionCmake) {
        $versionMatch = [regex]::Match((Read-TextSafe $versionCmake), 'set\(OFPS_RELEASE_VERSION "([^"]+)"\)')
        if ($versionMatch.Success) { $Context.Result.Version = $versionMatch.Groups[1].Value }
    }
}

# Expected destinations, whether or not there is a payload beside this script.
$expected = New-Object System.Collections.ArrayList
$null = $expected.Add(@{ Path = (Join-Safe $addonDir $Context.AddonFileName);     Rel = ('x64/' + $Context.AddonFileName) })
$null = $expected.Add(@{ Path = (Join-Safe $addonDir 'optimizer-fps-dlss5-core.dll'); Rel = 'x64/optimizer-fps-dlss5-core.dll' })
$null = $expected.Add(@{ Path = (Join-Safe $addonDir $Context.ForwarderFileName); Rel = ('x64/' + $Context.ForwarderFileName) })
foreach ($shaderRel in @($manifest.Keys | Where-Object { $_ -match '(?i)^x64/optimizer-fps-dlss5/[^/]+\.dxbc$' })) {
    $shaderPath = Join-Safe $addonDir ($shaderRel.Substring(4).Replace('/', '\'))
    $null = $expected.Add(@{ Path = $shaderPath; Rel = $shaderRel })
}
if ($arch -eq 'x86' -and $remoteDir) {
    $null = $expected.Add(@{ Path = (Join-Safe $remoteDir $Context.RemoteFileName); Rel = ('x86/' + $Context.RemoteFileName) })
}

$receiptHashes = @{}
if ($receipt -and $receipt.PSObject.Properties['Files']) {
    foreach ($f in @($receipt.Files)) { $receiptHashes[([string]$f.Path).ToLowerInvariant()] = [string]$f.InstalledHash }
}
if ($manifestSource -ne 'payload') {
    $shaderPrefix = (Join-Safe $addonDir $Context.ShaderFolderName).ToLowerInvariant().TrimEnd('\') + '\'
    foreach ($path in @($receiptHashes.Keys | Where-Object { $_.StartsWith($shaderPrefix) -and $_ -match '(?i)\.dxbc$' })) {
        $rel = 'x64/optimizer-fps-dlss5/' + [IO.Path]::GetFileName($path)
        if (@($expected | Where-Object { $_.Rel -ieq $rel }).Count -eq 0) {
            $null = $expected.Add(@{ Path = $path; Rel = $rel })
        }
    }
}

$missing = 0
$mismatched = 0
foreach ($e in $expected) {
    if (-not (Test-FileHere $e.Path)) {
        & $Context.Report -Status 'Fail' -Text ('Missing: ' + (Get-RelativePathCompat $gameRoot $e.Path))
        $missing++
        continue
    }
    $now = Get-Sha256 $e.Path
    $want = $null
    if ($manifestSource -eq 'payload' -and $manifest.ContainsKey($e.Rel)) { $want = $manifest[$e.Rel] }
    elseif ($receiptHashes.ContainsKey($e.Path.ToLowerInvariant())) { $want = $receiptHashes[$e.Path.ToLowerInvariant()] }
    if ($want -and -not [string]::Equals($now, $want, [StringComparison]::OrdinalIgnoreCase)) {
        $status = 'Warn'
        if ($e.Rel -match '(?i)\.dxbc$' -or $e.Rel -eq 'x64/optimizer-fps-dlss5-core.dll') { $status = 'Fail' }
        & $Context.Report -Status $status -Text ('Different from the reference: ' + [IO.Path]::GetFileName($e.Path))
        $mismatched++
    }
    else {
        & $Context.Report -Status 'Ok' -Text ([IO.Path]::GetFileName($e.Path) + ' present')
    }
}

$Context.Result.Static['MissingFiles'] = $missing
$Context.Result.Static['ChangedFiles'] = $mismatched
$Context.Result.Static['HashSource']   = $manifestSource

$corePath = Join-Safe $addonDir 'optimizer-fps-dlss5-core.dll'
$coreHash = $null
if ($manifestSource -eq 'payload' -and $manifest.ContainsKey('x64/optimizer-fps-dlss5-core.dll')) {
    $coreHash = $manifest['x64/optimizer-fps-dlss5-core.dll']
}
elseif ($receipt -and $receipt.PSObject.Properties['Core'] -and $receipt.Core -and
        $receipt.Core.PSObject.Properties['InstalledHash']) { $coreHash = [string]$receipt.Core.InstalledHash }
elseif ($receiptHashes.ContainsKey($corePath.ToLowerInvariant())) { $coreHash = $receiptHashes[$corePath.ToLowerInvariant()] }
$core = Get-OfpsCoreFileEvidence -CorePath $corePath -ExpectedVersion $Context.Result.Version `
    -ExpectedHash $coreHash -ReadPeInfo { param($p) Get-PeInfo $p } `
    -ReadExports { param($p) Get-PeExportNames $p } -ReadVersion { param($p) Get-FileVersionSafe $p }
if ($manifestSource -eq 'payload' -and -not $manifest.ContainsKey('x64/optimizer-fps-dlss5-core.dll')) {
    $core.StaticVerdict = 'invalid'
}
$versionTool = $Context.CoreVersionTool
if (-not $versionTool) {
    $candidate = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'out\build\x64\tests\Release\ofps_core_api_dll_tests.exe'
    if (Test-FileHere $candidate) { $versionTool = $candidate }
}
if ($core.Exists -and $versionTool -and (Test-FileHere $versionTool)) {
    $versionOutput = & $versionTool '--version' $corePath 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0 -and $versionOutput -match 'ABI=(\d+);release=([^\s]+)') {
        $core.AbiRuntime = [int]$Matches[1]
        $core.RuntimeRelease = $Matches[2]
        if ($core.AbiRuntime -ne 1 -or ($Context.Result.Version -and $core.RuntimeRelease -cne $Context.Result.Version)) {
            $core.StaticVerdict = 'invalid'
        }
    }
}
$Context.Result.Static['Core'] = $core
if ($core.StaticVerdict -ne 'ok') { & $Context.Report -Status 'Fail' -Text ('Core static evidence: ' + $core.StaticVerdict) }
else { & $Context.Report -Status 'Ok' -Text 'Core x64 PE and exports checked.' }
if (-not $Context.Result.Version -or -not $coreHash) {
    & $Context.Report -Status 'Warn' -Text 'Core version or hash has no trusted reference.'
}
if ($core.AbiRuntime -eq 'unverified') { & $Context.Report -Status 'Info' -Text 'ABI runtime unverified; no local version helper result.' }
if ($addonDir -ne $targetDir -and (Test-FileHere (Join-Safe $targetDir 'OptiScaler.ini'))) {
    $otherCore = Join-Safe $targetDir 'optimizer-fps-dlss5-core.dll'
    if (Test-FileHere $otherCore) {
        $otherVersion = Get-FileVersionSafe $otherCore
        $Context.Result.Static['HostCoreVersion'] = $otherVersion
        if ($core.FileVersion -and $otherVersion -cne $core.FileVersion) {
            & $Context.Report -Status 'Fail' -Text 'The two cores in this OptiScaler installation have different versions.'
        }
    }
}

# The forwarder must export the three entry points the add-on calls.
$fwd = Join-Safe $addonDir $Context.ForwarderFileName
if (Test-FileHere $fwd) {
    $names = Get-PeExportNames $fwd
    $ok = $false
    if ($null -ne $names) {
        $ok = ($names -contains 'pw_ngx_call_create') -and ($names -contains 'pw_ngx_call_evaluate') -and ($names -contains 'pw_ngx_call_release')
    }
    $Context.Result.Static['ForwarderExports'] = $ok
    if ($ok) { & $Context.Report -Status 'Ok' -Text 'The NGX forwarder exports pw_ngx_call_create/evaluate/release.' }
    else { & $Context.Report -Status 'Fail' -Text 'nvngx.dll_optimizerfps.dll does not export the entry points the add-on calls.' }
}

# The add-on itself.
$addonPath = Join-Safe $addonDir $Context.AddonFileName
if (Test-FileHere $addonPath) {
    $names = Get-PeExportNames $addonPath
    $isOurs = $false
    if ($null -ne $names) { $isOurs = ($names -contains 'OptimizerFpsSetSettingV1') -or
        (($names -contains 'PeripheralWarpSetLayoutV1') -and ($names -contains 'PeripheralWarpSetTemporalV1')) }
    $Context.Result.Static['AddonIsOurs'] = $isOurs
    $Context.Result.Static['AddonExports'] = @($names)
    if (-not $isOurs) { & $Context.Report -Status 'Fail' -Text 'optimizer-fps-dlss5.addon64 is not an Optimizer FPS build.' }
}

# Shaders.
$shaderDir = Join-Safe $addonDir $Context.ShaderFolderName
$shaderCount = 0
if (Test-DirHere $shaderDir) { $shaderCount = @(Get-ChildItem -LiteralPath $shaderDir -File -Filter '*.dxbc' -ErrorAction SilentlyContinue).Count }
$Context.Result.Static['ShaderCount'] = $shaderCount
$expectedShaders = @($expected | Where-Object { $_.Rel -match '(?i)^x64/optimizer-fps-dlss5/.+\.dxbc$' })
$wantShaders = $expectedShaders.Count
$Context.Result.Static['ExpectedShaderCount'] = $(if ($manifestSource) { $wantShaders } else { $null })
$Context.Result.Static['ExpectedShaders'] = @($expectedShaders | ForEach-Object { $_.Rel })
if ($manifestSource -and $wantShaders -eq 0) {
    & $Context.Report -Status 'Fail' -Text 'Reference contains no compiled shader names.'
}
$actualShaders = @()
if (Test-DirHere $shaderDir) { $actualShaders = @(Get-ChildItem -LiteralPath $shaderDir -File -Filter '*.dxbc' -ErrorAction SilentlyContinue) }
$extraShaders = @()
if ($wantShaders -gt 0) {
    $extraShaders = @($actualShaders | Where-Object { $name = $_.Name; @($expectedShaders | Where-Object { [IO.Path]::GetFileName($_.Path) -ieq $name }).Count -eq 0 } | ForEach-Object { $_.Name })
}
$Context.Result.Static['ExtraShaders'] = $extraShaders
if ($extraShaders.Count -gt 0) { & $Context.Report -Status 'Warn' -Text ('Extra compiled shaders: ' + ($extraShaders -join ', ')) }
if ($shaderCount -eq 0) {
    & $Context.Report -Status 'Fail' -Text ('No compiled shaders in ' + $shaderDir + '; the add-on will forward every frame untouched.')
}
elseif ($wantShaders -gt 0 -and $shaderCount -ne $wantShaders) {
    & $Context.Report -Status 'Warn' -Text ('' + $shaderCount + ' shader(s) present, the payload has ' + $wantShaders + '.')
}
elseif (-not $manifestSource) { & $Context.Report -Status 'Warn' -Text 'No manifest or receipt; full shader set cannot be confirmed.' }
else {
    & $Context.Report -Status 'Ok' -Text ('' + $shaderCount + ' compiled shader(s) in optimizer-fps-dlss5\.')
}

# Files under the names this add-on used before 26.26.
$legacyPresent = New-Object System.Collections.ArrayList
foreach ($n in @($Context.LegacyAddonFileName, $Context.LegacyForwarderFileName, $Context.LegacySessionMarker)) {
    $p = Join-Safe $addonDir $n
    if (Test-FileHere $p) { $null = $legacyPresent.Add($p) }
}
$legacyShaderDir = Join-Safe $addonDir $Context.LegacyShaderFolderName
if (Test-DirHere $legacyShaderDir) { $null = $legacyPresent.Add($legacyShaderDir) }
if ($remoteDir) {
    $p = Join-Safe $remoteDir $Context.LegacyRemoteFileName
    if (Test-FileHere $p) { $null = $legacyPresent.Add($p) }
}
$Context.Result.Static['LegacyFiles'] = @($legacyPresent.ToArray())
if ($legacyPresent.Count -gt 0) {
    & $Context.Report -Status 'Warn' -Text ('' + $legacyPresent.Count + ' legacy file(s) from an earlier release are still present; run Update.') `
           -Detail (($legacyPresent | ForEach-Object { [IO.Path]::GetFileName($_) }) -join "`n")
}

# ReShade build.
$reshadeVersion = Get-FileVersionSafe $reshadeDll
$hasAddons = Test-ReShadeHasAddons $reshadeDll
$Context.Result.Static['ReShadeVersion'] = $reshadeVersion
$Context.Result.Static['ReShadeAddons']  = $hasAddons
if ($hasAddons -eq $false) { & $Context.Report -Status 'Fail' -Text 'That ReShade build has no add-on support.' }
else {
    $vm = [regex]::Match([string]$reshadeVersion, '^\s*(\d+)\.(\d+)')
    $want = $Context.ReShadeMinVersion -split '\.'
    if ($vm.Success -and ([int]$vm.Groups[1].Value -lt [int]$want[0] -or ([int]$vm.Groups[1].Value -eq [int]$want[0] -and [int]$vm.Groups[2].Value -lt [int]$want[1]))) {
        & $Context.Report -Status 'Fail' -Text ('ReShade ' + $reshadeVersion + ' is too old (need ' + $Context.ReShadeMinVersion + '+).')
    }
    else {
        $vt = $reshadeVersion
        if (-not $vt) { $vt = 'unknown version' }
        & $Context.Report -Status 'Ok' -Text ('ReShade ' + $vt + ', add-on build.')
    }
}

& $Context.WriteSection 'Neural rendering'

$nrRuntime = $null
foreach ($d in @($gameRoot, $targetDir, $addonDir)) {
    if (-not (Test-DirHere $d)) { continue }
    $p = Join-Safe $d 'nvngx_dlssnr.dll'
    if (Test-FileHere $p) { $nrRuntime = $p; break }
}
if ($nrRuntime) {
    $nrv = Get-FileVersionSafe $nrRuntime
    $Context.Result.Static['NrRuntimeVersion'] = $nrv
    if (-not $nrv) { $nrv = 'unknown version' }
    & $Context.Report -Status 'Ok' -Text ('nvngx_dlssnr.dll ' + $nrv)
}
else { & $Context.Report -Status 'Warn' -Text 'nvngx_dlssnr.dll was not found beside the game.' }

$consumers = @()
foreach ($d in @($addonDir, $targetDir, $gameRoot, $remoteDir)) {
    if (-not (Test-DirHere $d)) { continue }
    foreach ($pat in @('renodx-dlss*.addon64', 'dlss5-feed*.addon64', 'dlss5-feed*.addon32', 'standalone-dlssnr.addon64', 'standalone-dlssnr.addon32')) {
        foreach ($h in @(Get-ChildItem -LiteralPath $d -File -Filter $pat -ErrorAction SilentlyContinue)) { $consumers += $h.Name }
    }
    $opti = Join-Safe $d 'OptiScaler.ini'
    if (Test-FileHere $opti) {
        $t = Read-TextSafe $opti
        if ($t -and $t -match '(?im)^\s*\[DlssNr\]') { $consumers += 'OptiScaler.ini [DlssNr]' }
    }
}
$consumers = @($consumers | Sort-Object -Unique)
$Context.Result.Static['Consumers'] = $consumers
if ($consumers.Count -gt 0) { & $Context.Report -Status 'Ok' -Text ('Consumer: ' + ($consumers -join ', ')) }
else { & $Context.Report -Status 'Warn' -Text 'No neural-rendering consumer found; nothing will call feature 18.' }

& $Context.WriteSection 'Settings'

$hasNew = Test-IniSection $iniText 'OptimizerFPS'
$hasOld = Test-IniSection $iniText 'PeripheralWarp'
$Context.Result.Static['OptimizerFPSSection'] = $hasNew
$Context.Result.Static['LegacyPeripheralWarpSection'] = $hasOld
if ($hasNew) {
    $m = Get-IniKey $iniText 'OptimizerFPS' 'Mode'
    $mt = $m
    if (-not $mt) { $mt = '(unset)' }
    & $Context.Report -Status 'Ok' -Text ('[OptimizerFPS] present, Mode=' + $mt)
    $Context.Result.Static['IniMode'] = $m
    if ($m -eq '0') { & $Context.Report -Status 'Warn' -Text 'Mode=0: the add-on is loaded but does nothing.' }
}
elseif ($hasOld) { & $Context.Report -Status 'Warn' -Text 'Legacy [PeripheralWarp] exists without [OptimizerFPS]; run Update.' }
else { & $Context.Report -Status 'Warn' -Text 'ReShade.ini has no [OptimizerFPS] section yet.' }
if ($hasOld -and $hasNew) { & $Context.Report -Status 'Warn' -Text 'Legacy [PeripheralWarp] remains beside [OptimizerFPS].' }

$disabledFound = @()
foreach ($pair in @(@{ Ini = $iniPath; Name = $Context.AddonName }, @{ Ini = (Join-Safe $remoteDir 'ReShade.ini'); Name = $Context.RemoteAddonName })) {
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
$Context.Result.Static['DisabledAddons'] = @($disabledFound)
if ($disabledFound.Count -gt 0) {
    & $Context.Report -Status 'Warn' -Text 'The add-on is listed in [ADDON] DisabledAddons; enable it in Home -> Add-ons.' -Detail ($disabledFound -join "`n")
}

$marker = Join-Safe $addonDir $Context.SessionMarker
$Context.Result.Static['StaleSessionMarker'] = (Test-FileHere $marker)
if (Test-FileHere $marker) {
    & $Context.Report -Status 'Warn' -Text ($Context.SessionMarker + ' is left over; the add-on will start in pass-through (crash guard).') `
           -Detail ('Delete ' + $marker + ', or press Retry in the add-on tab.')
}

$Context.Receipt = $receipt
$Context.Missing = $missing
}

Export-ModuleMember -Function Get-VerifyPeBits, Find-VerifyReShadeDll, Invoke-VerifyStatic

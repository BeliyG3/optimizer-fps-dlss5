#Requires -Version 5.1
Set-StrictMode -Version 2.0

function Get-OfpsCoreFileEvidence {
    param(
        [Parameter(Mandatory = $true)][string]$CorePath,
        [string]$ExpectedVersion,
        [string]$ExpectedHash,
        [Parameter(Mandatory = $true)][scriptblock]$ReadPeInfo,
        [Parameter(Mandatory = $true)][scriptblock]$ReadExports,
        [Parameter(Mandatory = $true)][scriptblock]$ReadVersion
    )
    $result = [ordered]@{
        Path = $CorePath; Exists = $false; Machine = $null; Exports = @()
        HasVersionExport = $false; HasCreateExport = $false
        FileVersion = $null; ExpectedVersion = $ExpectedVersion; VersionMatches = $null
        ExpectedHash = $ExpectedHash; ActualHash = $null; HashMatches = $null
        AbiRuntime = 'unverified'; RuntimeRelease = $null; StaticVerdict = 'missing'
    }
    if (-not (Test-Path -LiteralPath $CorePath -PathType Leaf)) { return [pscustomobject]$result }
    $result.Exists = $true
    $pe = & $ReadPeInfo $CorePath
    if ($null -ne $pe) { $result.Machine = $pe.Arch }
    $names = & $ReadExports $CorePath
    $result.Exports = @($names)
    $result.HasVersionExport = $names -contains 'OfpsCoreVersion'
    $result.HasCreateExport = $names -contains 'OfpsCreateCore'
    $result.FileVersion = & $ReadVersion $CorePath
    if ($ExpectedVersion) {
        $result.VersionMatches = [string]::Equals([string]$result.FileVersion,
            $ExpectedVersion, [StringComparison]::Ordinal)
    }
    $result.ActualHash = (Get-FileHash -LiteralPath $CorePath -Algorithm SHA256).Hash
    if ($ExpectedHash) {
        $result.HashMatches = [string]::Equals($result.ActualHash, $ExpectedHash,
            [StringComparison]::OrdinalIgnoreCase)
    }
    $valid = ($result.Machine -eq 'x64') -and $result.HasVersionExport -and $result.HasCreateExport
    if ($null -ne $result.VersionMatches) { $valid = $valid -and $result.VersionMatches }
    if ($null -ne $result.HashMatches) { $valid = $valid -and $result.HashMatches }
    if ($valid) { $result.StaticVerdict = 'ok' } else { $result.StaticVerdict = 'invalid' }
    return [pscustomobject]$result
}

function Get-OfpsCoreLogEvidence {
    param([AllowNull()][string]$LogText, [string]$ExpectedVersion, [bool]$Stale = $false)
    if ($Stale) { return [pscustomobject]@{ Verdict='stale-log'; Abi=$null; Release=$null; Reason=$null } }
    if (-not $LogText) { return [pscustomobject]@{ Verdict='unknown'; Abi=$null; Release=$null; Reason='no log' } }
    $fail = [regex]::Matches($LogText, '(?im)^.*Optimizer FPS: core load failed: ([^\r\n]+)$')
    $ok = [regex]::Matches($LogText, '(?im)^.*Optimizer FPS: core loaded; ABI (\d+); release ([^\r\n\s]+)')
    $failIndex = -1; $okIndex = -1
    if ($fail.Count -gt 0) { $failIndex = $fail[$fail.Count - 1].Index }
    if ($ok.Count -gt 0) { $okIndex = $ok[$ok.Count - 1].Index }
    if ($failIndex -gt $okIndex) {
        $reason = $fail[$fail.Count - 1].Groups[1].Value.Trim()
        $verdict = 'missing'
        if ($reason -match '(?i)another version|is version') { $verdict = 'version-mismatch' }
        elseif ($reason -match '(?i)ABI') { $verdict = 'abi-mismatch' }
        return [pscustomobject]@{ Verdict=$verdict; Abi=$null; Release=$null; Reason=$reason }
    }
    if ($okIndex -lt 0) { return [pscustomobject]@{ Verdict='unknown'; Abi=$null; Release=$null; Reason='no core load line' } }
    $abi = [int]$ok[$ok.Count - 1].Groups[1].Value
    $release = $ok[$ok.Count - 1].Groups[2].Value
    $verdict = 'loaded'
    if ($abi -ne 1) { $verdict = 'abi-mismatch' }
    elseif (-not $ExpectedVersion) { $verdict = 'unknown' }
    elseif ($ExpectedVersion -and $release -cne $ExpectedVersion) { $verdict = 'version-mismatch' }
    $reason = $null
    if ($verdict -eq 'unknown') { $reason = 'no trusted release reference' }
    return [pscustomobject]@{ Verdict=$verdict; Abi=$abi; Release=$release; Reason=$reason }
}

Export-ModuleMember -Function Get-OfpsCoreFileEvidence, Get-OfpsCoreLogEvidence

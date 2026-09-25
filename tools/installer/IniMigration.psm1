#Requires -Version 5.1
Set-StrictMode -Version 2.0

function Get-OfpsKnownIniKeys {
    $schema = @(
        'Mode', 'ColorFilter', 'CenterX', 'CenterY', 'WorkX', 'WorkY',
        'GlobalScale', 'Flags', 'OffsetX', 'OffsetY', 'WorkShiftX', 'WorkShiftY',
        'WorkShiftEnabled', 'ShowCenterOutline', 'ShowWorkOutline',
        'Brightness', 'Gamma', 'TemporalMode', 'TemporalEvery',
        'TemporalMaxQueue', 'ModelPasses', 'SpreadPasses',
        'DebugTiming', 'DebugTemporalReadback', 'DebugAsyncLog',
        'DebugAsyncShowPass', 'DebugAsyncCompute', 'DebugAsyncNormalPriority',
        'DebugAsyncNoRealtime', 'DebugHookDelayMs', 'DebugKeepBackbuffer',
        'DebugDepthState', 'DebugTemporalVis', 'DebugTemporalKeepOutput',
        'DebugTemporalBlend', 'DebugTemporalDepth', 'DebugTemporalSmooth',
        'DebugMotionSmooth', 'DebugPassNoHistory',
        'DebugTemporalNoModelMotion', 'DebugTemporalNoExpect',
        'DebugTemporalNoCells', 'DebugTemporalPhaseIn', 'DebugWarpPath'
    )
    # DebugLayer belongs to shell settings and must appear only once.
    return @($schema + @('Passive', 'FloatingWindow', 'TraceExit',
                         'DebugLayer', 'CrashGuard'))
}

function Get-OfpsIniSections {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text)
    $old = $false
    $new = $false
    foreach ($line in ($Text -split "`r?`n")) {
        $header = $line.Trim().TrimStart([char]0xFEFF)
        if ($header -match '^\[([^\]]+)\]$') {
            if ($Matches[1] -ieq 'PeripheralWarp') { $old = $true }
            if ($Matches[1] -ieq 'OptimizerFPS') { $new = $true }
        }
    }
    return [pscustomobject]@{ Old = $old; New = $new }
}

function Get-OfpsIniValue {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text,
        [Parameter(Mandatory = $true)][string]$Section,
        [Parameter(Mandatory = $true)][string]$Key
    )
    $active = $false
    $found = $null
    foreach ($line in ($Text -split "`r?`n")) {
        $trimmed = $line.Trim().TrimStart([char]0xFEFF)
        if ($trimmed -match '^\[([^\]]+)\]$') {
            $active = $Matches[1] -ieq $Section
            continue
        }
        if (-not $active -or $trimmed -match '^[;#]') { continue }
        if ($trimmed -match '^([^=]+?)\s*=\s*(.*)$' -and
            $Matches[1].Trim() -ieq $Key) {
            $found = $Matches[2].Trim()
        }
    }
    return $found
}

function Convert-OfpsIniForUpdate {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text)

    $sections = Get-OfpsIniSections -Text $Text
    if (-not $sections.Old) {
        return [pscustomobject]@{
            Text = $Text
            Changed = $false
            Outcome = 'none'
            MovedNames = @()
        }
    }

    $known = @{}
    foreach ($key in @(Get-OfpsKnownIniKeys)) {
        $known[$key.ToLowerInvariant()] = $key
    }
    $moved = New-Object System.Collections.Specialized.OrderedDictionary(
        [StringComparer]::OrdinalIgnoreCase)
    $parts = New-Object System.Collections.ArrayList
    $inOld = $false
    # Match includes each line ending; other sections are emitted byte-for-byte.
    foreach ($match in [regex]::Matches($Text, '[^\r\n]*(?:\r\n|\n|\r|$)')) {
        $part = $match.Value
        if ($part.Length -eq 0) { continue }
        $trimmed = $part.Trim().TrimStart([char]0xFEFF)
        if ($trimmed -match '^\[([^\]]+)\]$') {
            $inOld = $Matches[1] -ieq 'PeripheralWarp'
            if (-not $inOld) { $null = $parts.Add($part) }
            continue
        }
        if (-not $inOld) { $null = $parts.Add($part); continue }
        if ($sections.New) { continue }
        if ($trimmed -match '^[;#]' -or $trimmed -notmatch '=') { continue }
        $equalAt = $trimmed.IndexOf('=')
        $key = $trimmed.Substring(0, $equalAt).Trim()
        $value = $trimmed.Substring($equalAt + 1).Trim()
        $lookup = $key.ToLowerInvariant()
        if ($known.ContainsKey($lookup)) { $moved[$known[$lookup]] = $value }
    }

    $result = @($parts.ToArray()) -join ''
    if ($Text.Length -gt 0 -and $Text[0] -eq [char]0xFEFF -and
        ($result.Length -eq 0 -or $result[0] -ne [char]0xFEFF)) {
        $result = [char]0xFEFF + $result
    }
    $outcome = 'new-wins'
    if (-not $sections.New) {
        $outcome = 'old-only'
        $newline = "`r`n"
        if ($Text -notmatch "`r`n" -and $Text -match "`n") { $newline = "`n" }
        if ($result.Length -gt 0 -and -not $result.EndsWith("`n") -and
            -not $result.EndsWith("`r")) { $result += $newline }
        if ($result.Length -gt 0) { $result += $newline }
        $result += '[OptimizerFPS]' + $newline
        foreach ($key in $moved.Keys) {
            $result += ([string]$key + '=' + [string]$moved[$key] + $newline)
        }
    }
    return [pscustomobject]@{
        Text = $result
        Changed = $true
        Outcome = $outcome
        MovedNames = @($moved.Keys)
    }
}

function Convert-OfpsIniOwnership {
    param(
        [object[]]$Previous,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$OldText,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$NewText,
        [Parameter(Mandatory = $true)][ValidateSet('none','old-only','new-wins')]
        [string]$Outcome
    )
    $owned = New-Object System.Collections.ArrayList
    $seen = @{}
    foreach ($entry in @($Previous)) {
        if ($null -eq $entry -or [string]$entry.Path -ine $Path) { continue }
        $section = [string]$entry.Section
        $key = [string]$entry.Key
        $value = [string]$entry.Value
        if ($section -ieq 'PeripheralWarp') {
            if ($Outcome -ne 'old-only') { continue }
            if ((Get-OfpsIniValue $OldText 'PeripheralWarp' $key) -cne $value) {
                continue
            }
            if ((Get-OfpsIniValue $NewText 'OptimizerFPS' $key) -cne $value) {
                continue
            }
            $section = 'OptimizerFPS'
        }
        elseif ($section -ieq 'OptimizerFPS') {
            if ((Get-OfpsIniValue $NewText 'OptimizerFPS' $key) -cne $value) {
                continue
            }
        }
        else { continue }
        $id = ($Path + '|' + $section + '|' + $key).ToLowerInvariant()
        if ($seen.ContainsKey($id)) { continue }
        $seen[$id] = $true
        $null = $owned.Add([ordered]@{
            Path = $Path; Section = 'OptimizerFPS'; Key = $key; Value = $value
        })
    }
    return @($owned.ToArray())
}

Export-ModuleMember -Function Get-OfpsKnownIniKeys, Get-OfpsIniSections, `
    Get-OfpsIniValue, Convert-OfpsIniForUpdate, Convert-OfpsIniOwnership

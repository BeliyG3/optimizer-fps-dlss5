#Requires -Version 5.1

# ReShade.ini is user data. Decode strictly and keep its UTF-8 BOM and line endings.
function Read-OfpsIniDocument
{
    param([string] $Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        return [pscustomobject]@{ Exists = $false; Text = ''; Encoding = (New-Object Text.UTF8Encoding($false)); Hash = $null }
    }
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -ge 2 -and (($bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) -or
        ($bytes[0] -eq 0xFE -and $bytes[1] -eq 0xFF))) {
        throw "Unsupported ReShade.ini encoding: $Path"
    }
    $bom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    $offset = if ($bom) { 3 } else { 0 }
    $decoder = New-Object Text.UTF8Encoding($false, $true)
    $text = $decoder.GetString($bytes, $offset, $bytes.Length - $offset)
    return [pscustomobject]@{
        Exists = $true
        Text = $text
        Encoding = (New-Object Text.UTF8Encoding($bom))
        Hash = (Get-Sha256 $Path)
    }
}

function Test-OfpsIniBackups
{
    param([object[]] $Changes, [string] $BackupDir, [string] $PrimaryPath)
    foreach ($change in $Changes) {
        if (-not $change.Document.Exists) { continue }
        $tag = if ([string]::Equals($change.Path, $PrimaryPath, [StringComparison]::OrdinalIgnoreCase)) {
            'ReShade.ini.original'
        } else { 'ReShade.ini.remote.original' }
        $backup = Join-Path $BackupDir $tag
        if (-not (Test-Path -LiteralPath $backup -PathType Leaf) -or
            (Get-Sha256 $backup) -ne $change.Document.Hash) {
            throw "ReShade.ini backup does not match source: $($change.Path)"
        }
        $change.BackupPath = $backup
    }
}

function Write-OfpsIniTransaction
{
    param([object[]] $Changes, [hashtable] $Context)
    $written = New-Object System.Collections.ArrayList
    try {
        foreach ($change in $Changes) {
            if ($change.NewText -ceq $change.Document.Text) { continue }
            Write-TextAtomic -Content $change.NewText -Path $change.Path -Context $Context -Encoding $change.Document.Encoding
            $null = $written.Add($change)
        }
    }
    catch {
        $failure = $_.Exception.Message
        foreach ($change in @($written.ToArray())) {
            if ($change.Document.Exists) {
                Copy-FileAtomic -Source $change.BackupPath -Destination $change.Path `
                    -ExpectedSha256 $change.Document.Hash -Context $Context
            }
            elseif (Test-Path -LiteralPath $change.Path) {
                Remove-Item -LiteralPath $change.Path -Force
            }
        }
        throw "ReShade.ini write failed; earlier ini files restored: $failure"
    }
}

Export-ModuleMember -Function Read-OfpsIniDocument, Test-OfpsIniBackups, Write-OfpsIniTransaction

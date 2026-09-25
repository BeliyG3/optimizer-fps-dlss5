#Requires -Version 5.1
param(
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$Root
)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$lint = Join-Path $RepoRoot 'tools\Lint-PowerShell51.ps1'
$cases = @(
    @{Name='relative-path-api'; Source='[IO.Path]::GetRelativePath("a", "b")'},
    @{Name='hex-api'; Source='[Convert]::ToHexString([byte[]]@(1))'},
    @{Name='json-hashtable'; Source='"{}" | ConvertFrom-Json -AsHashtable'},
    @{Name='null-coalescing'; Source='$x = $null ?? 1'},
    @{Name='null-conditional'; Source='$x = $item?.Name'},
    @{Name='ternary'; Source='$x = $true ? 1 : 2'},
    @{Name='join-path-positional'; Source='Join-Path "a" "b" "c"'}
)
$lintRoot = Join-Path $Root 'lint-micro'
New-Item -ItemType Directory -Path $lintRoot | Out-Null
foreach ($case in $cases) {
    $file = Join-Path $lintRoot ($case.Name + '.ps1')
    [IO.File]::WriteAllText($file, "#Requires -Version 5.1`r`n" + $case.Source + "`r`n")
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $lint -Path $file 2>&1 | Out-String
    $code = $LASTEXITCODE
    $ErrorActionPreference = $oldPreference
    Check ('lint rejects ' + $case.Name) ($code -ne 0 -and $output -match 'finding\(s\)') $output
}

$nested = Join-Path $lintRoot 'nested'
New-Item -ItemType Directory -Path $nested | Out-Null
$module = Join-Path $nested 'Invalid.psm1'
$test = Join-Path $nested 'Invalid.Tests.ps1'
[IO.File]::WriteAllText($module, "#Requires -Version 5.1`r`n[IO.Path]::GetRelativePath('a','b')`r`n")
[IO.File]::WriteAllText($test, "#Requires -Version 5.1`r`n[Convert]::ToHexString([byte[]]@(1))`r`n")
$oldPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $lint -Path $nested 2>&1 | Out-String
$code = $LASTEXITCODE
$ErrorActionPreference = $oldPreference
Check 'lint recursively checks modules and nested tests' `
    ($code -ne 0 -and $output -match 'checking 2 file\(s\)' -and
     $output -match 'Invalid\.psm1' -and $output -match 'Invalid\.Tests\.ps1') $output

$clean = Join-Path $lintRoot 'Clean.ps1'
[IO.File]::WriteAllText($clean, @'
#Requires -Version 5.1
# [IO.Path]::GetRelativePath and ?? in a comment are harmless.
$message = '[Convert]::ToHexString and $item?.Name are text'
Write-Output $message
'@)
$output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $lint -Path $clean 2>&1 | Out-String
Check 'lint ignores comments and string contents' ($LASTEXITCODE -eq 0) $output

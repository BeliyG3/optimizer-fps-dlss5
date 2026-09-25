#Requires -Version 5.1
param([hashtable] $Context)
$script:TestContext = $Context

function Say { param([string] $T, [string] $C = 'Gray') Write-Host $T -ForegroundColor $C }

function Check
{
    param([string] $Name, [bool] $Ok, [string] $Detail)
    if ($Ok) { $script:TestContext.Pass++; Say ('  [PASS] ' + $Name) 'Green' }
    else     { $script:TestContext.Fail++; Say ('  [FAIL] ' + $Name) 'Red'; if ($Detail) { Say ('         ' + $Detail) 'DarkGray' } }
    $null = $script:TestContext.Rows.Add([pscustomobject]@{ Test = $Name; Result = $(if ($Ok) { 'PASS' } else { 'FAIL' }); Detail = $Detail })
}

function Skip
{
    param([string] $Name, [string] $Why)
    $script:TestContext.Skip++
    Say ('  [SKIP] ' + $Name + ' -- ' + $Why) 'Yellow'
    $null = $script:TestContext.Rows.Add([pscustomobject]@{ Test = $Name; Result = 'SKIP'; Detail = $Why })
}

function Invoke-Installer
{
    param([string[]] $Arguments)
    $all = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script:TestContext.Installer) + $Arguments
    $out = & $script:TestContext.PsExe @all 2>&1 | Out-String
    $code = $LASTEXITCODE
    if ($null -eq $code) { $code = 0 }
    return [pscustomobject]@{ Code = $code; Out = $out }
}

function Invoke-Verifier
{
    param([string[]] $Arguments)
    $all = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script:TestContext.Verifier) + $Arguments
    $out = & $script:TestContext.PsExe @all 2>&1 | Out-String
    $code = $LASTEXITCODE
    if ($null -eq $code) { $code = 0 }
    return [pscustomobject]@{ Code = $code; Out = $out }
}

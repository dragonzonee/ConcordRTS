<#
.SYNOPSIS
  Runs the autoplay test scripts in tests\autoplay and prints a summary.
.EXAMPLE
  powershell -File tools\autoplay\Run-Autoplay.ps1
  powershell -File tools\autoplay\Run-Autoplay.ps1 -Script build_base
#>
param(
    [string]$Script = '*',
    [string]$OutRoot = 'build\autoplay\latest',
    [int]$TimeoutSeconds = 600
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Push-Location $root
try {
    $exe = 'bin\concord_runtime.exe'
    if (-not (Test-Path $exe)) { throw "Missing $exe - build the game first (tools\Build-Game.ps1)." }
    $scripts = Get-ChildItem 'tests\autoplay' -Filter "$Script.ini" | Sort-Object Name
    if (-not $scripts) { throw "No script matches tests\autoplay\$Script.ini" }

    $rows = @()
    foreach ($s in $scripts) {
        $out = Join-Path $OutRoot $s.BaseName
        New-Item -ItemType Directory -Force $out | Out-Null
        $resultFile = Join-Path $out 'result.txt'
        if (Test-Path $resultFile) { Clear-Content $resultFile }
        $argLine = "--config local_data\runtime_config.ini --resource-root RedAlert2 --local-data-root local_data " +
                "--autoplay `"$($s.FullName)`" --autoplay-out `"$out`""
        $p = Start-Process $exe -ArgumentList $argLine -PassThru
        if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
            $p.Kill()
            $rows += [pscustomobject]@{ Script = $s.BaseName; Result = 'TIMEOUT'; Steps = ''; Seconds = $TimeoutSeconds; Failure = 'process timeout' }
            continue
        }
        $info = @{}
        if (Test-Path $resultFile) {
            $lines = Get-Content $resultFile
            $info.result = $lines[0]
            foreach ($l in $lines | Select-Object -Skip 1) { $k, $v = $l -split '=', 2; $info[$k] = $v }
        }
        $rows += [pscustomobject]@{
            Script  = $s.BaseName
            Result  = if ($info.result) { $info.result } else { "NO RESULT (exit $($p.ExitCode))" }
            Steps   = $info.steps_passed
            Seconds = if ($info.seconds) { [math]::Round([double]$info.seconds, 1) } else { '' }
            Failure = $info.failure
        }
    }
    $rows | Format-Table -AutoSize -Wrap
    $failed = @($rows | Where-Object { $_.Result -ne 'PASS' }).Count
    Write-Host ("{0}/{1} passed. Logs and screenshots: {2}" -f ($rows.Count - $failed), $rows.Count, (Resolve-Path $OutRoot))
    exit ([int]($failed -gt 0))
} finally {
    Pop-Location
}

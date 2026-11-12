# usage: sweep.ps1 -list <file with "asset|class" lines> -tag <name> [-Extra "<editor args>"] [-Project <uproject>] [-OutDir <dir>] [-batch 10]
# Material / MIC sweep (sweep.py): one editor start per batch of assets, a crash is attributed to the first asset without result.
param($list, $tag, $Extra = "", $Project = "", $OutDir = "", $batch = 10)
. "$PSScriptRoot\_local.ps1"
$E = Get-SweepEnv $Project
if (-not $OutDir) { $OutDir = Join-Path $E.ProjectDir 'Saved\SuzieSweeps' }
$lines = Get-Content $list
$all = @(); $cls = @{}; foreach ($l in $lines) { if (-not $l.Trim()) { continue }; $a = $l -split '\|'; $all += $a[0]; $cls[$a[0]] = $a[1] }
$results = @{}; $idx = 0; $launches = 0
$logdir = Join-Path $OutDir "$tag.logs"; Remove-Item $logdir -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force $logdir | Out-Null
[Environment]::SetEnvironmentVariable('UE-LocalDataCachePath', $E.Ddc, 'Process')
while ($idx -lt $all.Count) {
  $end = [Math]::Min($idx + $batch, $all.Count) - 1; $chunk = @($all[$idx..$end]); $launches++
  $lf = "$logdir\list_$launches.txt"; $pf = "$logdir\prog_$launches.txt"; $chunk | Set-Content $lf
  [Environment]::SetEnvironmentVariable('SUZIE_LIST', $lf, 'Process'); [Environment]::SetEnvironmentVariable('SUZIE_PROG', $pf, 'Process')
  $a = @($E.Project) + $E.BaseArgs + @("-ExecutePythonScript=$PSScriptRoot\sweep.py") + (Split-Args $Extra)
  $p = Start-Process -FilePath $E.Exe -ArgumentList $a -PassThru
  if (-not $p.WaitForExit(240000)) { $p.Kill() }
  Copy-Item $E.LogFile "$logdir\log_$launches.txt" -Force
  $processed = 0
  if (Test-Path $pf) { foreach ($l in Get-Content $pf) { $a2 = $l -split '\|'; if ($a2[0] -eq 'OK') { $results[$a2[1]] = @('PASS', ''); $processed++ } elseif ($a2[0] -eq 'FAIL') { $results[$a2[1]] = @('FAIL', $a2[2]); $processed++ } } }
  if ($processed -lt $chunk.Count) {
    $first = $chunk[$processed]
    $err = (Select-String -Path "$logdir\log_$launches.txt" -Pattern 'Assertion failed|Unhandled Exception|Fatal error:|Ensure condition failed' | Select -First 1).Line
    if (-not $err) { $err = 'process ended without DONE (timeout/early exit)' }
    $results[$first] = @('CRASH', $err.Substring(0, [Math]::Min(240, $err.Length))); $processed++
  }
  $idx += $processed
  "launch=$launches idx=$idx results=$($results.Count)" | Add-Content "$logdir\driver.log"
}
$out = foreach ($k in $all) { [pscustomobject]@{asset = $k; class = $cls[$k]; status = $results[$k][0]; first_error = ($results[$k][1] -replace '[\r\n,]', ' ') } }
$out | Export-Csv (Join-Path $OutDir "$tag.results.csv") -NoTypeInformation
$out | Group-Object status | % { "$($_.Name): $($_.Count)" }

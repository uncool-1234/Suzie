# usage: runlist.ps1 -list <file, one asset per line (a "|Type" suffix is ignored)> -tag <name> [-Extra "<editor args>"] [-Project <uproject>] [-OutDir <dir>] [-timeout 1500]
# Blueprint / AnimBP / Widget load sweep (m_test.py) in ONE editor session; after a crash records the culprit and restarts with the rest.
param($list, $tag, $Extra = "", $Project = "", $OutDir = "", $timeout = 1500)
. "$PSScriptRoot\_local.ps1"
$E = Get-SweepEnv $Project
if (-not $OutDir) { $OutDir = Join-Path $E.ProjectDir 'Saved\SuzieSweeps' }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$out = $OutDir
Remove-Item "$out\$tag.done", "$out\$tag.results.csv" -ErrorAction SilentlyContinue
$rem = [System.Collections.ArrayList]@(Get-Content $list | ? { $_.Trim() } | % { ($_ -split '\|')[0].Trim() })
$n = 0; $res = @()
[Environment]::SetEnvironmentVariable('UE-LocalDataCachePath', $E.Ddc, 'Process')
[Environment]::SetEnvironmentVariable('SUZIE_GC', '', 'Process')
while ($rem.Count -gt 0 -and $n -lt 700) {
  $n++
  $lf = "$out\$tag.L$n.list.txt"; $pf = "$out\$tag.L$n.prog.txt"; Remove-Item $pf -ErrorAction SilentlyContinue
  $rem | Set-Content $lf
  [Environment]::SetEnvironmentVariable('SUZIE_LIST', $lf, 'Process'); [Environment]::SetEnvironmentVariable('SUZIE_PROG', $pf, 'Process')
  $a = @($E.Project) + $E.BaseArgs + @("-ExecutePythonScript=$PSScriptRoot\m_test.py") + (Split-Args $Extra)
  $p = Start-Process -FilePath $E.Exe -ArgumentList $a -PassThru
  if (-not $p.WaitForExit($timeout * 1000)) { $p.Kill() }
  Copy-Item $E.LogFile "$out\$tag.L$n.log" -Force
  $done = @{}
  if (Test-Path $pf) { foreach ($l in Get-Content $pf) { $x = $l -split '\|'; if ($x[0] -eq 'OK') { $done[$x[1]] = @('PASS', ($x[2..($x.Count - 1)] -join '|')) } elseif ($x[0] -eq 'FAIL') { $done[$x[1]] = @('FAIL', $x[2]) } } }
  $new = [System.Collections.ArrayList]@()
  foreach ($r in $rem) { if ($done.ContainsKey($r)) { $res += [pscustomobject]@{asset = $r; status = $done[$r][0]; info = $done[$r][1] } } else { [void]$new.Add($r) } }
  if ($new.Count -gt 0) {
    $first = $new[0]
    $err = (Select-String -Path "$out\$tag.L$n.log" -Pattern 'Fatal error|Assertion failed|Unhandled Exception' | Select -First 1).Line
    if (-not $err) { $m2 = Select-String -Path "$out\$tag.L$n.log" -Pattern 'ObjectSerializationError' | Select -First 1; if ($m2) { $err = $m2.Line } }
    if (-not $err) { $err = 'process ended without result (timeout/early exit)' }
    $res += [pscustomobject]@{asset = $first; status = 'CRASH'; info = $err.Substring(0, [Math]::Min(300, $err.Length)) }
    $new.RemoveAt(0)
  }
  $rem = $new
  $res | Export-Csv "$out\$tag.results.csv" -NoTypeInformation
  Get-Process UnrealEditor -ErrorAction SilentlyContinue | % { if ($_.Id -eq $p.Id) { $_.Kill() } }
}
$res | Export-Csv "$out\$tag.results.csv" -NoTypeInformation
"FINISHED" | Add-Content "$out\$tag.done"

# usage: runlist.ps1 -list <file> -tag <name> [-data <json>] [-batchtimeout 400]
# Runs the editor over a list of assets; on a crash records the culprit and continues with the rest.
param($list,$tag,$data="",$timeout=1500)
$out='<path>'; New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item "$out\$tag.done","$out\$tag.results.csv" -ErrorAction SilentlyContinue
$rem = [System.Collections.ArrayList]@(Get-Content $list | ? { $_.Trim() })
$n=0; $res=@()
[Environment]::SetEnvironmentVariable('UE-LocalDataCachePath','<path>','Process')
[Environment]::SetEnvironmentVariable('Game_GC','',  'Process')
while ($rem.Count -gt 0 -and $n -lt 700) {
  $n++
  $lf="$out\$tag.L$n.list.txt"; $pf="$out\$tag.L$n.prog.txt"; Remove-Item $pf -ErrorAction SilentlyContinue
  $rem | Set-Content $lf
  [Environment]::SetEnvironmentVariable('Game_LIST',$lf,'Process'); [Environment]::SetEnvironmentVariable('Game_PROG',$pf,'Process')
  $a = @('<path>','-log','-d3d12','-sm5','-nosplash','-unattended','-ModkitKeyFile=<path>','-ExecutePythonScript=<path>')
  if ($data) { $a += "-GameNativeData=$data" }
  if ($env:Game_EXTRA) { $a += ($env:Game_EXTRA -split ' ' | ? { $_ }) }
  $p=Start-Process -FilePath '<path>' -ArgumentList $a -PassThru
  if (-not $p.WaitForExit($timeout*1000)) { $p.Kill() }
  Copy-Item <path> "$out\$tag.L$n.log" -Force
  $done=@{}; $dn=$false
  if (Test-Path $pf) { foreach ($l in Get-Content $pf) { $x=$l -split '\|'; if ($x[0] -eq 'OK') { $done[$x[1]]=@('PASS',($x[2..($x.Count-1)] -join '|')) } elseif ($x[0] -eq 'FAIL') { $done[$x[1]]=@('FAIL',$x[2]) } elseif ($x[0] -eq 'DONE') { $dn=$true } } }
  $new=[System.Collections.ArrayList]@()
  foreach ($r in $rem) { if ($done.ContainsKey($r)) { $res += [pscustomobject]@{asset=$r;status=$done[$r][0];info=$done[$r][1]} } else { [void]$new.Add($r) } }
  if ($new.Count -gt 0) {
    $first=$new[0]
    $err=(Select-String -Path "$out\$tag.L$n.log" -Pattern 'Fatal error|Assertion failed|Unhandled Exception' | Select -First 1).Line
    if (-not $err) { $m2 = Select-String -Path "$out\$tag.L$n.log" -Pattern 'ObjectSerializationError' | Select -First 1; if ($m2) { $err=$m2.Line } }
    if (-not $err) { $err='process ended without result (timeout/early exit)' }
    $res += [pscustomobject]@{asset=$first;status='CRASH';info=$err.Substring(0,[Math]::Min(300,$err.Length))}
    $new.RemoveAt(0)
  }
  $rem = $new
  $res | Export-Csv "$out\$tag.results.csv" -NoTypeInformation
  Get-Process UnrealEditor -ErrorAction SilentlyContinue | % { if ($_.Id -eq $p.Id) { $_.Kill() } }
}
$res | Export-Csv "$out\$tag.results.csv" -NoTypeInformation
"FINISHED" | Add-Content "$out\$tag.done"

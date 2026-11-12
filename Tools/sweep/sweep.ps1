$lines = Get-Content <path>
$all = @(); $cls=@{}; foreach ($l in $lines) { $a=$l -split '\|'; $all += $a[0]; $cls[$a[0]]=$a[1] }
$results=@{}; $idx=0; $batch=10; $launches=0
$logdir='<path>'; Remove-Item $logdir -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force $logdir | Out-Null
[Environment]::SetEnvironmentVariable('UE-LocalDataCachePath','<path>','Process')
while ($idx -lt $all.Count) {
  $end=[Math]::Min($idx+$batch,$all.Count)-1; $chunk=@($all[$idx..$end]); $launches++
  $lf="$logdir\list_$launches.txt"; $pf="$logdir\prog_$launches.txt"; $chunk | Set-Content $lf
  [Environment]::SetEnvironmentVariable('Game_LIST',$lf,'Process'); [Environment]::SetEnvironmentVariable('Game_PROG',$pf,'Process')
  $p=Start-Process -FilePath '<path>' -ArgumentList '<path>','-log','-d3d12','-sm5','-nosplash','-unattended','-ModkitKeyFile=<path>','-ExecutePythonScript=<path>' -PassThru
  if (-not $p.WaitForExit(240000)) { $p.Kill() }
  Copy-Item <path> "$logdir\log_$launches.txt" -Force
  $processed=0
  if (Test-Path $pf) { foreach ($l in Get-Content $pf) { $a=$l -split '\|'; if ($a[0] -eq 'OK') { $results[$a[1]]=@('PASS',''); $processed++ } elseif ($a[0] -eq 'FAIL') { $results[$a[1]]=@('FAIL',$a[2]); $processed++ } } }
  if ($processed -lt $chunk.Count) {
    $first=$chunk[$processed]
    $err=(Select-String -Path "$logdir\log_$launches.txt" -Pattern 'Assertion failed|Unhandled Exception|Fatal error:|Ensure condition failed' | Select -First 1).Line
    if (-not $err) { $err='process ended without DONE (timeout/early exit)' }
    $results[$first]=@('CRASH',$err.Substring(0,[Math]::Min(240,$err.Length))); $processed++
  }
  $idx += $processed
  "launch=$launches idx=$idx results=$($results.Count)" | Add-Content "$logdir\driver.log"
}
$out = foreach ($k in $all) { [pscustomobject]@{asset=$k; class=$cls[$k]; status=$results[$k][0]; first_error=($results[$k][1] -replace '[\r\n,]',' ')} }
$out | Export-Csv <path> -NoTypeInformation
$out | Group-Object status | % { "$($_.Name): $($_.Count)" }

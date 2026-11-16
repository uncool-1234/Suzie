# usage: sweep.ps1 -list <file with "asset|class" lines> -tag <name> [-Extra "<editor args>"] [-Project <uproject>] [-OutDir <dir>] [-timeout 1500]
# Material / MIC sweep (sweep.py) in ONE editor session; after a crash the asset that was being rendered is recorded and the session restarts with the rest.
# Status: PASS (rendered and read back), FAIL (Python exception), CRASH. Per asset also: error lines in its log slice, pixels of the 128x128 capture that
# differ from a capture without the sphere (0 = nothing visible) and the largest channel difference.
param($list, $tag, $Extra = "", $Project = "", $OutDir = "", $timeout = 1500)
. "$PSScriptRoot\_local.ps1"
$E = Get-SweepEnv $Project
if (-not $OutDir) { $OutDir = Join-Path $E.ProjectDir 'Saved\SuzieSweeps' }
$all = @(); $cls = @{}
foreach ($l in Get-Content $list) { if (-not $l.Trim()) { continue }; $a = $l -split '\|'; $all += $a[0]; $cls[$a[0]] = $a[1] }
$logdir = Join-Path $OutDir "$tag.logs"; Remove-Item $logdir -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force $logdir | Out-Null
[Environment]::SetEnvironmentVariable('UE-LocalDataCachePath', $E.Ddc, 'Process')
$noise = 'DirectoryWatcher|Failed to read file|DerivedDataCache|XGE|LogStats|CVar|LogGameplayTags: Warning|Script Stack|LogShaderCompilers|LogMemoryImage|LogMaterial: Warning|LogTexture|LogSlate|LogAudio'
$bad = 'Error:|Fatal|Ensure condition failed|Assertion failed|Unhandled Exception|Invalid uniform buffer'
$results = @{}; $errs = @{}; $rem = $all; $launches = 0
while ($rem.Count -gt 0 -and $launches -lt 300) {
  $launches++
  $lf = "$logdir\list_$launches.txt"; $pf = "$logdir\prog_$launches.txt"; $rem | Set-Content $lf
  [Environment]::SetEnvironmentVariable('SUZIE_LIST', $lf, 'Process'); [Environment]::SetEnvironmentVariable('SUZIE_PROG', $pf, 'Process')
  $a = @($E.Project) + $E.BaseArgs + @("-ExecutePythonScript=$PSScriptRoot\sweep.py") + (Split-Args $Extra)
  $p = Start-Process -FilePath $E.Exe -ArgumentList $a -PassThru
  if (-not $p.WaitForExit($timeout * 1000)) { $p.Kill() }
  $log = "$logdir\log_$launches.txt"; Copy-Item $E.LogFile $log -Force
  # error lines per asset, between its SZMAT START and END markers (to the end of the log for an asset that crashed)
  $cur = $null
  foreach ($l in [IO.File]::ReadLines($log)) {
    if ($l -match 'SZMAT\|START\|(\S+)') { $cur = $matches[1]; $errs[$cur] = @(); continue }
    if ($l -match 'SZMAT\|END\|') { $cur = $null; continue }
    if ($cur -and $l -match $bad -and $l -notmatch $noise) { $errs[$cur] += ($l -replace '^\[[^\]]*\]\[[^\]]*\]', '') }
  }
  $started = $null
  if (Test-Path $pf) {
    foreach ($l in Get-Content $pf) {
      $x = $l -split '\|'
      switch ($x[0]) {
        'START' { $started = $x[1] }
        'OK'    { $results[$x[1]] = @('PASS', '', $x[2], $x[3]); $started = $null }
        'FAIL'  { $results[$x[1]] = @('FAIL', $x[2], '', ''); $started = $null }
      }
    }
  }
  $rest = @($rem | Where-Object { -not $results.ContainsKey($_) })
  if ($rest.Count -gt 0) {
    $culprit = if ($started) { $started } else { $rest[0] }
    $err = (Select-String -Path $log -Pattern 'Assertion failed|Unhandled Exception|Fatal error:|Ensure condition failed' | Select -First 1).Line
    if (-not $err) { $err = 'process ended without result (timeout/early exit)' }
    $results[$culprit] = @('CRASH', $err.Substring(0, [Math]::Min(240, $err.Length)), '', '')
    $rest = @($rest | Where-Object { $_ -ne $culprit })
  }
  $rem = $rest
  "launch=$launches done=$($results.Count) remaining=$($rem.Count)" | Add-Content "$logdir\driver.log"
  Get-Process UnrealEditor -ErrorAction SilentlyContinue | Where-Object { $_.Id -eq $p.Id } | ForEach-Object { $_.Kill() }
}
$out = foreach ($k in $all) {
  $r = $results[$k]; $e = @($errs[$k] | Where-Object { $_ })
  $firstLine = if ($e.Count) { $e[0] -replace '[\r\n,]', ' ' } else { '' }
  [pscustomobject]@{ asset = $k; class = $cls[$k]; status = $r[0]; first_error = ($r[1] -replace '[\r\n,]', ' ');
                     error_lines = $e.Count; first_error_line = $firstLine; changed_pixels = $r[2]; peak_change = $r[3] }
}
$out | Export-Csv (Join-Path $OutDir "$tag.results.csv")
$out | Group-Object status | ForEach-Object { "$($_.Name): $($_.Count)" }
"launches: $launches"
"with error lines: $(@($out | Where-Object { $_.error_lines -gt 0 }).Count)"
"nothing visible: $(@($out | Where-Object { $_.status -eq 'PASS' -and $_.changed_pixels -eq '0' }).Count)"

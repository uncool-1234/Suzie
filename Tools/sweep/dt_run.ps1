# usage: dt_run.ps1 -Tag <name> [-Extra "<editor args>"] [-Project <uproject>] [-OutDir <dir>] [-Max 300] [-Restarts 8]
# Loads up to -Max DataTable assets (dt_test.py) in one editor session; after a crash the culprit (last START without OK) is excluded and the
# session restarts (max -Restarts). Results: <OutDir>\<Tag>.dt.csv (asset,status,row struct), summary line on stdout.
param($Tag, $Extra = "", $Project = "", $OutDir = "", $Max = 300, $Restarts = 8)
. "$PSScriptRoot\_local.ps1"
$E = Get-SweepEnv $Project
if (-not $OutDir) { $OutDir = Join-Path $E.ProjectDir 'Saved\SuzieSweeps' }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$excl = "$OutDir\$Tag.dt.exclude.txt"; Remove-Item $excl -ErrorAction SilentlyContinue
[Environment]::SetEnvironmentVariable('UE-LocalDataCachePath', $E.Ddc, 'Process')
$rows = @(); $crashed = @()
for ($n = 1; $n -le $Restarts + 1; $n++) {
  $res = "$OutDir\$Tag.dt$n.txt"; Remove-Item $res -ErrorAction SilentlyContinue
  [Environment]::SetEnvironmentVariable('SUZIE_DT_OUT', $res, 'Process'); [Environment]::SetEnvironmentVariable('SUZIE_DT_MAX', "$Max", 'Process')
  [Environment]::SetEnvironmentVariable('SUZIE_DT_EXCLUDE', $excl, 'Process')
  $a = @($E.Project) + $E.BaseArgs + @("-ExecutePythonScript=$PSScriptRoot\dt_test.py") + (Split-Args $Extra)
  $p = Start-Process -FilePath $E.Exe -ArgumentList $a -PassThru
  $null = $p.Handle
  if (-not $p.WaitForExit(1500000)) { $p.Kill() }
  Copy-Item $E.LogFile "$OutDir\$Tag.dt$n.log" -Force
  $lines = Get-Content $res
  $done = $lines -contains 'DONE'
  $started = $lines | Where-Object { $_ -like 'START|*' } | ForEach-Object { $_.Substring(6) }
  $okset = @{}; foreach ($l in $lines) { $x = $l -split '\|'; if ($x[0] -eq 'OK' -or $x[0] -eq 'FAIL') { $okset[$x[1]] = $true; $rows += [pscustomobject]@{asset = $x[1]; status = $x[0]; info = ($x[2..($x.Count - 1)] -join '|')} } }
  if ($done) { break }
  $culprit = $started | Where-Object { -not $okset.ContainsKey($_) } | Select-Object -Last 1
  if (-not $culprit) { break }
  $crashed += $culprit; $culprit | Add-Content $excl
  $rows += [pscustomobject]@{asset = $culprit; status = 'CRASH'; info = (Select-String -Path "$OutDir\$Tag.dt$n.log" -Pattern 'Assertion failed|Fatal error' | Select -First 1).Line }
  Get-Process UnrealEditor -ErrorAction SilentlyContinue | ForEach-Object { if ($_.Id -eq $p.Id) { $_.Kill() } }
}
$rows = @($rows | Sort-Object asset -Unique)
$rows | Export-Csv "$OutDir\$Tag.dt.csv" -NoTypeInformation
"DT: ok=$(@($rows | Where-Object status -eq 'OK').Count) fail=$(@($rows | Where-Object status -eq 'FAIL').Count) crash=$($crashed.Count) :: $($crashed -join ', ')"

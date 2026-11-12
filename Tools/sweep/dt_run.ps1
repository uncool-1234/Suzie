# usage: dt_run.ps1 -Tag <name> [-Extra "<editor args>"] [-Project <uproject>] [-OutDir <dir>] [-Max 300]
# One editor start: loads up to -Max DataTable assets (dt_test.py). Result file <OutDir>\<Tag>.dt.txt (DONE line present = no crash).
param($Tag, $Extra = "", $Project = "", $OutDir = "", $Max = 300)
. "$PSScriptRoot\_local.ps1"
$E = Get-SweepEnv $Project
if (-not $OutDir) { $OutDir = Join-Path $E.ProjectDir 'Saved\SuzieSweeps' }
New-Item -ItemType Directory -Force $OutDir | Out-Null
Remove-Item "$OutDir\$Tag.dt.txt" -ErrorAction SilentlyContinue
[Environment]::SetEnvironmentVariable('UE-LocalDataCachePath', $E.Ddc, 'Process')
[Environment]::SetEnvironmentVariable('SUZIE_DT_OUT', "$OutDir\$Tag.dt.txt", 'Process'); [Environment]::SetEnvironmentVariable('SUZIE_DT_MAX', "$Max", 'Process')
$a = @($E.Project) + $E.BaseArgs + @("-ExecutePythonScript=$PSScriptRoot\dt_test.py") + (Split-Args $Extra)
$p = Start-Process -FilePath $E.Exe -ArgumentList $a -PassThru
$null = $p.Handle
if (-not $p.WaitForExit(1200000)) { $p.Kill() }
Copy-Item $E.LogFile "$OutDir\$Tag.dt.log" -Force
Get-Content "$OutDir\$Tag.dt.txt" | Select-String 'FOUND|SUMMARY|DONE'

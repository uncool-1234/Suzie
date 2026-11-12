# usage: golden.ps1 -Tag <name> -Out <dump file> [-Extra "<more editor args>"] [-Project <uproject>] [-LogDir <dir>]
# One editor start: golden.py runs verify + tag probe + Suzie.DumpAll, the log is copied to <LogDir>\<Tag>.log.
param($Tag, $Out, $Extra = "", $Project = "", $LogDir = "")
. "$PSScriptRoot\_local.ps1"
$E = Get-SweepEnv $Project
if (-not $LogDir) { $LogDir = Join-Path $E.ProjectDir 'Saved\SuzieSweeps' }
New-Item -ItemType Directory -Force $LogDir | Out-Null
[Environment]::SetEnvironmentVariable('UE-LocalDataCachePath', $E.Ddc, 'Process')   # isolated DDC
[Environment]::SetEnvironmentVariable('SUZIE_DUMP_OUT', $Out, 'Process')
$a = @($E.Project) + $E.BaseArgs + @("-ExecutePythonScript=$PSScriptRoot\golden.py") + (Split-Args $Extra)
$p = Start-Process -FilePath $E.Exe -ArgumentList $a -PassThru
$null = $p.Handle
if (-not $p.WaitForExit(900000)) { $p.Kill(); "TIMEOUT" | Set-Content "$LogDir\$Tag.timeout" }
"exit=" + $p.ExitCode | Set-Content "$LogDir\$Tag.done"
Copy-Item $E.LogFile "$LogDir\$Tag.log" -Force

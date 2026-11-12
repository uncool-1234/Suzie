# usage: golden.ps1 -Tag <name> -Out <dump file> [-Extra "<more editor args>"]
# Starts one editor on the project, runs golden.py (Suzie.DumpAll), copies the log to <LogDir>\<Tag>.log.
param($Tag, $Out, $Extra = "", $Project = "<path>", $LogDir = "<path>", $KeyFile = "<path>", $Engine = "<path>")
New-Item -ItemType Directory -Force $LogDir | Out-Null
[Environment]::SetEnvironmentVariable('UE-LocalDataCachePath', "$(Split-Path $Project)\DDC", 'Process')
[Environment]::SetEnvironmentVariable('SUZIE_DUMP_OUT', $Out, 'Process')
$a = @($Project, '-log', '-d3d12', '-sm5', '-nosplash', '-unattended', "-ModkitKeyFile=$KeyFile", "-ExecutePythonScript=$PSScriptRoot\golden.py")
if ($Extra) { $a += ($Extra -split ' ' | ? { $_ }) }
$p = Start-Process -FilePath $Engine -ArgumentList $a -PassThru
$null = $p.Handle
if (-not $p.WaitForExit(900000)) { $p.Kill(); "TIMEOUT" | Set-Content "$LogDir\$Tag.timeout" }
"exit=" + $p.ExitCode | Set-Content "$LogDir\$Tag.done"
Copy-Item "$(Split-Path $Project)\Saved\Logs\ModProject.log" "$LogDir\$Tag.log" -Force

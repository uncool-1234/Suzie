# usage: run_gates.ps1 -Tag <name> [-Project <uproject>] [-OutDir <dir>] [-Extra "<editor args>"] [-OldGolden <file>] [-Skip golden,bp,mat]
# The regression gates in one go (golden dump + verify, Blueprint sweep, material sweep), with the generator selected by -Extra (or by the project/profile):
#   golden   one editor start: verify, tags, Suzie.DumpAll; log excerpt: override counters, verify totals
#   bp       Blueprint/AnimBP/Widget load sweep (the single blueprints_*.list in the sweeps folder)
#   mat      material / MIC sweep (the single materials_*.list in the sweeps folder)
# Expected values are in the profile's README.
# Machine independent: project from -Project / $env:MODKIT_PROJECT, lists from -Sweeps (default: <Profile>\sweeps, Profile= of the project's ModkitLocal.ini).
param($Tag, $Project = "", $OutDir = "", $Extra = "", $OldGolden = "", $Sweeps = "", [string[]]$Skip = @())
. "$PSScriptRoot\_local.ps1"
$E = Get-SweepEnv $Project
if (-not $OutDir) { $OutDir = Join-Path $E.ProjectDir 'Saved\SuzieSweeps' }
New-Item -ItemType Directory -Force $OutDir | Out-Null
if (-not $Sweeps) {
  if (-not $E.Local['Profile']) { throw 'pass -Sweeps, or set Profile= in the project''s ModkitData\ModkitLocal.ini' }
  $Sweeps = Join-Path $E.Local['Profile'] 'sweeps'
}
function Get-SweepList($pattern) {
  $l = @(Get-ChildItem $Sweeps -Filter $pattern -File)
  if ($l.Count -ne 1) { throw "expected one $pattern in $Sweeps, found $($l.Count)" }
  $l[0].FullName
}
$sum = [ordered]@{ tag = $Tag; extra = $Extra }
if ($Skip -notcontains 'golden') {
  & "$PSScriptRoot\golden.ps1" -Tag "$Tag.golden" -Out "$OutDir\$Tag.golden" -Extra $Extra -Project $E.Project -LogDir $OutDir
  $log = Get-Content "$OutDir\$Tag.golden.log"
  $sum.overrides = ($log | Select-String 'Schema overrides:' | Select -First 1).Line -replace '^.*LogSuzie: ', ''
  $sum.generate = ($log | Select-String 'GENERATE ' | Select -First 1).Line -replace '^.*LogSuzie: ', ''
  $sum.verify = ($log | Select-String 'LogSuzie: VERIFY RESULT: types=' | Select -Last 1).Line -replace '^.*LogSuzie: ', ''
  if ($OldGolden) { python "$PSScriptRoot\golden_diff.py" $OldGolden "$OutDir\$Tag.golden" | Set-Content "$OutDir\$Tag.golden.diff.txt"; $sum.golden_diff = Get-Content "$OutDir\$Tag.golden.diff.txt" -Tail 1 }
}
if ($Skip -notcontains 'bp') {
  $bpList = Get-SweepList 'blueprints_*.list'
  & "$PSScriptRoot\runlist.ps1" -list $bpList -tag "$Tag.bp" -Extra $Extra -Project $E.Project -OutDir $OutDir
  $sum.bp = (python "$PSScriptRoot\make_sweep_csv.py" "$Tag.bp" "$OutDir\$Tag.bp.csv" --dir $OutDir --list $bpList) -join ' '
}
if ($Skip -notcontains 'mat') {
  $sum.mat = (& "$PSScriptRoot\sweep.ps1" -list (Get-SweepList 'materials_*.list') -tag "$Tag.mat" -Extra $Extra -Project $E.Project -OutDir $OutDir) -join ' '
}
$sum | ConvertTo-Json | Set-Content "$OutDir\$Tag.summary.json"
$sum | ConvertTo-Json

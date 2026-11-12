# usage: run_gates.ps1 -Tag <name> [-Project <uproject>] [-OutDir <dir>] [-Extra "<editor args>"] [-OldGolden <file>] [-Skip golden,bp,mat]
# The parity gates of the modkit architecture (G2/G4) in one go, with the generator selected by -Extra (or by the project/profile):
#   golden   one editor start: verify, tags, Suzie.DumpAll; log excerpt: override counters, verify totals
#   bp       Blueprint/AnimBP/Widget load sweep (Profiles/<profile>/sweeps/blueprints_531.list)   expected: CLEAN >= 529, DIRTY <= 2, CRASH 0
#   mat      material / MIC sweep (materials_200.list)                                            expected: PASS 200
# Machine independent: project from -Project / $env:MODKIT_PROJECT, lists from -Sweeps (default: the profile dir next to this script's plugin).
param($Tag, $Project = "", $OutDir = "", $Extra = "", $OldGolden = "", $Sweeps = "", [string[]]$Skip = @())
. "$PSScriptRoot\_local.ps1"
$E = Get-SweepEnv $Project
if (-not $OutDir) { $OutDir = Join-Path $E.ProjectDir 'Saved\SuzieSweeps' }
New-Item -ItemType Directory -Force $OutDir | Out-Null
if (-not $Sweeps) { $Sweeps = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'Profiles\Game\sweeps' }
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
  & "$PSScriptRoot\runlist.ps1" -list "$Sweeps\blueprints_531.list" -tag "$Tag.bp" -Extra $Extra -Project $E.Project -OutDir $OutDir
  $sum.bp = (python "$PSScriptRoot\make_sweep_csv.py" "$Tag.bp" "$OutDir\$Tag.bp.csv" --dir $OutDir --list "$Sweeps\blueprints_531.list") -join ' '
}
if ($Skip -notcontains 'mat') {
  $sum.mat = (& "$PSScriptRoot\sweep.ps1" -list "$Sweeps\materials_200.list" -tag "$Tag.mat" -Extra $Extra -Project $E.Project -OutDir $OutDir) -join ' '
}
$sum | ConvertTo-Json | Set-Content "$OutDir\$Tag.summary.json"
$sum | ConvertTo-Json

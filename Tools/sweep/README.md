Sweep harness (machine independent: nothing here contains a path of the dev PC).

Common arguments: `-Project <path to the .uproject>` (or env `MODKIT_PROJECT`), `-Extra "<more editor args>"`, `-OutDir <dir>` (default `<ProjectDir>\Saved\SuzieSweeps`).
The engine comes from `[Local] EngineDir=` of `<ProjectDir>\ModkitData\ModkitLocal.ini` (else the path in the .uproject `EngineAssociation`), the key file from `[Local] KeyFile=`
(only its path is passed as `-ModkitKeyFile=`). The DDC is isolated per project (`<ProjectDir>\DDC`). One editor at a time (GPU, DDC, project lock).

- `run_gates.ps1 -Tag t [-OldGolden file]`: golden dump + Blueprint sweep + material sweep, writes `<Tag>.summary.json`.
- `golden.ps1 -Tag t -Out file [-TimeoutSec s]`: one editor start (killed after `-TimeoutSec`, default 900), `golden.py` runs verify + tag probe + `Suzie.DumpAll`. `golden_diff.py old new [--structure] [--whitelist file]`
  compares two dumps block by block (`--structure` ignores sizes/offsets/flags); exit code 1 when a difference is not whitelisted.
- `runlist.ps1 -list <asset list> -tag t`: Blueprint/AnimBP/Widget load sweep in one editor session (restarts after a crash and records the culprit); the list may have `|Type` suffixes
  (`Profiles/Game/sweeps/blueprints_531.list`). `make_sweep_csv.py <tag> <out.csv> --dir <OutDir> --list <list>` classifies CLEAN / DIRTY (loads, error lines in its slice) / CRASH.
- `sweep.ps1 -list <asset|class list> -tag t`: material / MIC sweep in one editor session (create a MIC, assign it to a sphere, render offscreen and read the image back;
  restarts after a crash and records the culprit; `Profiles/Game/sweeps/materials_200.list`). The CSV adds error lines per asset and the pixels that differ from a capture without the sphere.
- `dt_run.ps1 -Tag t -Max 300`: loads random DataTable assets from the asset registry (`dt_test.py`), restarts after a crash and excludes the culprit.

Sweep harness (copies of the scripts used for the standalone generator; machine paths are those of the dev PC).

- `golden.ps1 -Tag t -Out file -Extra "-SuzieGenerate -SuzieProfile=Game ..."`: one editor start, `golden.py` runs verify + tag probe + `Suzie.DumpAll`.
- `runlist.ps1 -list <plain asset list> -tag t` with env `Game_EXTRA="-SuzieGenerate -SuzieProfile=Game -DisableGameNativeTypes"`:
  Blueprint/AnimBP/Widget load sweep (list: `Profiles/Game/sweeps/blueprints_531.list`, strip the `|Type` suffix first); `make_sweep_csv.py <tag> <out.csv>` classifies CLEAN/DIRTY/CRASH.
- `sweep.ps1` + `sweep.py`: material / MIC sweep (list `Profiles/Game/sweeps/materials_200.list`); the copy writes next to the original, edit `$logdir`/output paths before use.

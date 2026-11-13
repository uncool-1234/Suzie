# Shared helpers of the sweep harness. Nothing here is machine specific: the project comes from -Project / $env:MODKIT_PROJECT, everything else from
# <ProjectDir>\ModkitLocal.ini ([Local] EngineDir, KeyFile; the engine dir may also be the path in the .uproject EngineAssociation).
function Read-Ini($path) {
  $h = @{}
  if (Test-Path $path) { foreach ($l in Get-Content $path) { if ($l -match '^\s*([^;\[#=][^=]*)=(.*)$') { $h[$matches[1].Trim()] = $matches[2].Trim() } } }
  $h
}
function Get-SweepEnv($Project) {
  if (-not $Project) { $Project = $env:MODKIT_PROJECT }
  if (-not $Project) { throw 'pass -Project <path to the .uproject> or set MODKIT_PROJECT' }
  $dir = Split-Path -Parent (Resolve-Path $Project)
  $loc = Read-Ini (Join-Path $dir 'ModkitLocal.ini')
  $engine = $loc['EngineDir']
  if (-not $engine) { $ea = (Get-Content $Project -Raw | ConvertFrom-Json).EngineAssociation; if ($ea) { if (-not [IO.Path]::IsPathRooted($ea)) { $ea = Join-Path $dir $ea }; if (Test-Path $ea) { $engine = (Resolve-Path $ea).Path } } }
  if (-not $engine) { throw 'engine dir unknown: set EngineDir= in ModkitLocal.ini or use a path as EngineAssociation' }
  $name = [IO.Path]::GetFileNameWithoutExtension($Project)
  $args = @('-log', '-d3d12', '-sm5', '-nosplash', '-unattended')
  if ($loc['KeyFile']) { $args += "-ModkitKeyFile=$($loc['KeyFile'])" }   # a path to the key file, never the key
  [pscustomobject]@{ Project = (Resolve-Path $Project).Path; ProjectDir = $dir; Exe = (Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor.exe');
                     Ddc = (Join-Path $dir 'DDC'); LogFile = (Join-Path $dir "Saved\Logs\$name.log"); BaseArgs = $args; Local = $loc }
}
function Split-Args($s) { if ($s) { $s -split ' ' | Where-Object { $_ } } else { @() } }

# Shared helpers of the sweep harness. Nothing here is machine specific: the project comes from -Project / $env:MODKIT_PROJECT, everything else from
# <ProjectDir>\ModkitData\ModkitLocal.ini ([Local] EngineDir, KeyFile; the engine dir may also be the path in the .uproject EngineAssociation).
function Read-Ini($path) {
  $h = @{}
  if (Test-Path $path) { foreach ($l in Get-Content $path) { if ($l -match '^\s*([^;\[#=][^=]*)=(.*)$') { $h[$matches[1].Trim()] = $matches[2].Trim() } } }
  $h
}
# Sections the editor adds to a project's Config on its first start; a fresh copy of the template gets them too.
$EditorWrittenSections = @('[/Script/AndroidFileServerEditor.AndroidFileServerRuntimeSettings]')
function Get-IniText($path) {
  $sec = ''; $keep = $true; $out = New-Object System.Collections.ArrayList
  foreach ($l in Get-Content $path) {
    $t = $l.Trim()
    if (-not $t) { continue }
    if ($t -match '^\[.*\]$') { $sec = $t; $keep = $EditorWrittenSections -notcontains $t; if ($keep) { [void]$out.Add($t) }; continue }
    if ($keep) { [void]$out.Add($t) }
  }
  $out -join "`n"
}
# Warns when the project's Config differs from the profile's project template: results of such a project (gate values, golden dumps)
# may not match a project made from the template. Generated files (the tag list and DefaultGameplayTags.ini) are not compared.
function Test-TemplateDrift($dir, $loc) {
  $prof = $loc['Profile']
  if (-not $prof -or -not (Test-Path (Join-Path $prof 'packaging.json'))) { return }
  $rel = (Get-Content (Join-Path $prof 'packaging.json') -Raw | ConvertFrom-Json).template
  if (-not $rel) { return }
  $tpl = Join-Path $prof $rel
  if (-not (Test-Path $tpl)) { return }
  $tpl = (Resolve-Path $tpl).Path
  if ($tpl -eq $dir) { return }
  $generated = @('Config/DefaultGameplayTags.ini')
  $tagsJson = Join-Path $prof 'tags\tags.json'
  if (Test-Path $tagsJson) { $generated += (Get-Content $tagsJson -Raw | ConvertFrom-Json).tags_ini }
  $files = @{}
  foreach ($root in @($dir, $tpl)) {
    $cfg = Join-Path $root 'Config'
    if (Test-Path $cfg) { Get-ChildItem $cfg -Recurse -File | ForEach-Object { $files['Config/' + $_.FullName.Substring($cfg.Length + 1).Replace('\', '/')] = 1 } }
  }
  $msgs = @()
  foreach ($f in ($files.Keys | Sort-Object)) {
    if ($generated -contains $f) { continue }
    $d = Join-Path $dir $f; $t = Join-Path $tpl $f
    if (-not (Test-Path $t)) { $msgs += "only in the project: $f" }
    elseif (-not (Test-Path $d)) { $msgs += "missing in the project: $f" }
    elseif ((Get-IniText $d) -ne (Get-IniText $t)) { $msgs += "differs from the template: $f" }
  }
  if ($msgs.Count) { Write-Warning ("$dir differs from the project template $tpl; results may not match a project made from the template:`n  " + ($msgs -join "`n  ")) }
}
function Get-SweepEnv($Project) {
  if (-not $Project) { $Project = $env:MODKIT_PROJECT }
  if (-not $Project) { throw 'pass -Project <path to the .uproject> or set MODKIT_PROJECT' }
  $dir = Split-Path -Parent (Resolve-Path $Project)
  $loc = Read-Ini (Join-Path $dir 'ModkitData\ModkitLocal.ini')
  $engine = $loc['EngineDir']
  if (-not $engine) { $ea = (Get-Content $Project -Raw | ConvertFrom-Json).EngineAssociation; if ($ea) { if (-not [IO.Path]::IsPathRooted($ea)) { $ea = Join-Path $dir $ea }; if (Test-Path $ea) { $engine = (Resolve-Path $ea).Path } } }
  if (-not $engine) { throw 'engine dir unknown: set EngineDir= in ModkitData\ModkitLocal.ini or use a path as EngineAssociation' }
  Test-TemplateDrift $dir $loc
  $name = [IO.Path]::GetFileNameWithoutExtension($Project)
  $args = @('-log', '-d3d12', '-sm5', '-nosplash', '-unattended')
  if ($loc['KeyFile']) { $args += "-ModkitKeyFile=$($loc['KeyFile'])" }   # a path to the key file, never the key
  [pscustomobject]@{ Project = (Resolve-Path $Project).Path; ProjectDir = $dir; Exe = (Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor.exe');
                     Ddc = (Join-Path $dir 'DDC'); LogFile = (Join-Path $dir "Saved\Logs\$name.log"); BaseArgs = $args; Local = $loc }
}
function Split-Args($s) { if ($s) { $s -split ' ' | Where-Object { $_ } } else { @() } }

# Separation rule: no game/studio name in the generic plugin source. Exit 1 on a hit.
$root = Split-Path $PSScriptRoot
$hits = Get-ChildItem "$root\Source" -Recurse -File | Select-String -Pattern 'Game|Studio|Game|Game|Studio'
if ($hits) { $hits | % { "$($_.Path):$($_.LineNumber): $($_.Line.Trim())" }; exit 1 }
"check-layers: clean"

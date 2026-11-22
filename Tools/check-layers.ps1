# Separation rule: no game/studio name in any tracked file of this repository (this script excepted). Exit 1 on a hit.
$root = Split-Path $PSScriptRoot
$self = 'Tools/check-layers.ps1'
$files = @(git -C $root ls-files | Where-Object { $_ -ne $self } | ForEach-Object { Join-Path $root $_ })
$hits = Select-String -LiteralPath $files -Pattern 'Game|Studio|Game|Game|Studio'
if ($hits) { $hits | % { "$($_.Path):$($_.LineNumber): $($_.Line.Trim())" }; exit 1 }
"check-layers: clean ($($files.Count) files)"

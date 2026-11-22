# Separation rule: no game or studio name in any tracked file of this repository. Exit 1 on a hit.
# -Pattern: regular expression of the names to reject, kept outside this repository (for example -Pattern 'Name1|Name2').
param([Parameter(Mandatory = $true)][string]$Pattern)
$root = Split-Path $PSScriptRoot
$files = @(git -C $root ls-files | ForEach-Object { Join-Path $root $_ })
$hits = Select-String -LiteralPath $files -Pattern $Pattern
if ($hits) { $hits | % { "$($_.Path):$($_.LineNumber): $($_.Line.Trim())" }; exit 1 }
"check-layers: clean ($($files.Count) files)"

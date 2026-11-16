# Applies exact search/replace edits to files, each of which must match
# exactly once (or -Count times). Keeps each file's line endings.
#   .\tools\apply_edits.ps1 -Spec edits.json
# Spec: [ { "file": "...", "old": "...", "new": "...", "count": 1 }, ... ]
param([Parameter(Mandatory)] [string]$Spec)
$ErrorActionPreference = "Stop"
$edits = Get-Content $Spec -Raw | ConvertFrom-Json
$utf8 = New-Object Text.UTF8Encoding $false
$failed = 0
foreach ($e in $edits) {
    $path = $e.file
    if (-not [IO.Path]::IsPathRooted($path)) { $path = Join-Path (Get-Location).Path $path }
    $text = [IO.File]::ReadAllText($path)
    $crlf = $text.Contains("`r`n")
    $old = $e.old -replace "`r`n", "`n"
    $new = $e.new -replace "`r`n", "`n"
    if ($crlf) { $old = $old -replace "`n", "`r`n"; $new = $new -replace "`n", "`r`n" }
    $want = if ($e.count) { [int]$e.count } else { 1 }
    $n = ([regex]::Matches($text, [regex]::Escape($old))).Count
    if ($n -ne $want) {
        Write-Output "FAIL ($n matches, want $want): $path :: $($e.old.Split("`n")[0])"
        $failed++
        continue
    }
    [IO.File]::WriteAllText($path, $text.Replace($old, $new), $utf8)
}
Write-Output "$($edits.Count - $failed) applied, $failed failed"

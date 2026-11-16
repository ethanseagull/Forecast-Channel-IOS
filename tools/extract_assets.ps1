# Unpacks the user's Forecast Channel contents (HAFE v7, e.g. from nus_fetch.ps1)
# into the port's data directory. Each U8 content (06..0a) becomes
# content/0000000N/<path>. Files stay byte-identical (no decompression): the
# channel decompresses and parses them itself, as on the Wii.
#   .\tools\extract_assets.ps1 [-Contents HAFE_v7] [-Data build\windows\data]
param(
    [string]$Contents = "HAFE_v7",
    [string]$Data = "build\windows\data"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $dtk = "forecast-decomp\build\tools\dtk.exe"
    if (-not (Test-Path $dtk)) { throw "Build the decomp once first (it provides dtk)." }
    $out = Join-Path $Data "content"
    if (Test-Path $out) { Remove-Item $out -Recurse -Force }
    New-Item -ItemType Directory -Force $out | Out-Null
    foreach ($n in "06", "07", "08", "09", "0a") {
        $app = Join-Path $Contents "000000$n.app"
        if (-not (Test-Path $app)) { throw "Missing $app (the HAFE v7 channel contents)." }
        & $dtk vfs cp --no-decompress -q "${app}:" (Join-Path $out "000000$n\")
        if ($LASTEXITCODE) { throw "dtk failed on $app" }
    }
    $count = (Get-ChildItem $out -Recurse -File).Count
    "assets: $count files in $out"
} finally {
    Pop-Location
}

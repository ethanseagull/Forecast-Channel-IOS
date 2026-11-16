# Fetches the Forecast Channel decompilation at the commit this port targets and
# applies patches/decomp-port.patch (all changes are under #ifdef FORECAST_PORT,
# so the decomp's matching Wii build is unaffected).
#
# Then copy your own HAFE v7 main executable (00000001.app, SHA-1 3c3bdec3...)
# into forecast-decomp/orig/HAFE/ and build the decomp once to generate the
# binary tables the channel embeds:
#   cd forecast-decomp; py configure.py; py -m ninja
param(
    # Use an existing local clone instead of GitHub (works offline).
    [string]$Source = "https://github.com/doldecomp/forecast.git"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$decomp = Join-Path $root "forecast-decomp"
$commit = "92a5782"

if (-not (Test-Path $decomp)) {
    git clone --quiet $Source $decomp
    if ($LASTEXITCODE) { throw "git clone failed" }
}
git -C $decomp checkout --quiet $commit
if ($LASTEXITCODE) { throw "git checkout failed" }
git -C $decomp apply --ignore-whitespace --whitespace=nowarn (Join-Path $root "patches\decomp-port.patch")
if ($LASTEXITCODE) { throw "patch did not apply (already applied? run 'git -C forecast-decomp checkout .' first)" }
"forecast-decomp ready at $commit with patches/decomp-port.patch applied"

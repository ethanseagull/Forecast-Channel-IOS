# Packs the user's channel files and texture packs into forecast-data.zip, for
# an iOS build that needs no Files app: the iOS workflow puts the zip's
# channel/ and textures/ folders into the app bundle when the private repo has
# it as the asset of its "data" release (port/sdk/import.c and
# port/gx/gx_texpack.cpp read them from there).
#
#   .\tools\pack_data.ps1 [-Channel HAFE_v7] [-Textures build\windows\data\textures] [-Out build\ios\forecast-data.zip]
#   .\tools\pack_data.ps1 -Upload      # also replaces the "data" release asset (gh)
#
# Texture packs are compressed first (tools/texpack_compress: BC1 .dds where
# the app would compress anyway). The zip holds Nintendo data: it never goes
# into the repository, only into the private repo's release, and the .ipa
# built from it is for the user's own phone.
param(
    [string]$Channel = "HAFE_v7",
    [string]$Textures = "build\windows\data\textures",
    [string]$Out = "build\ios\forecast-data.zip",
    [switch]$Upload
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$names = "00000001.app", "00000006.app", "00000007.app", "00000008.app", "00000009.app", "0000000a.app"
foreach ($n in $names) {
    if (-not (Test-Path (Join-Path $Channel $n))) {
        throw "$Channel\$n is missing"
    }
}

$stage = Join-Path $env:TEMP "forecast-data-stage"
if (Test-Path $stage) {
    Remove-Item -Recurse -Force $stage
}
New-Item -ItemType Directory -Force (Join-Path $stage "channel") | Out-Null
foreach ($n in $names) {
    Copy-Item (Join-Path $Channel $n) (Join-Path $stage "channel\$n")
}

if (Test-Path $Textures) {
    $tool = Join-Path $env:TEMP "forecast-texpack-tool"
    if (-not (Test-Path (Join-Path $tool "texpack_compress.exe"))) {
        & $env:ComSpec /c "tools\texpack_compress\build.bat `"$tool`""
        if ($LASTEXITCODE -ne 0) { throw "texpack_compress build failed" }
    }
    & (Join-Path $tool "texpack_compress.exe") (Resolve-Path $Textures) (Join-Path $stage "textures")
    if ($LASTEXITCODE -ne 0) { throw "texpack_compress failed" }
} else {
    Write-Host "No texture packs at $Textures; the zip holds the channel files only."
}

$outDir = Split-Path -Parent $Out
if ($outDir) {
    New-Item -ItemType Directory -Force $outDir | Out-Null
}
if (Test-Path $Out) {
    Remove-Item -Force $Out
}
# Entries named with "/" (Windows PowerShell's CreateFromDirectory writes
# "\", which unzip on the macOS runner rejects).
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open((Join-Path (Get-Location) $Out), [System.IO.Compression.ZipArchiveMode]::Create)
try {
    Get-ChildItem -Recurse -File $stage | ForEach-Object {
        $rel = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $_.FullName, $rel,
            [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally {
    $zip.Dispose()
}
Remove-Item -Recurse -Force $stage
$mb = [math]::Round((Get-Item $Out).Length / 1MB, 1)
Write-Host "Wrote $Out ($mb MB)"

if ($Upload) {
    gh release view data *> $null
    if ($LASTEXITCODE -ne 0) {
        gh release create data --title "Channel data (private)" `
            --notes "The user's channel files and texture packs for the iOS build. Nintendo data: keep this repository private." | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "gh release create failed" }
    }
    gh release upload data $Out --clobber
    if ($LASTEXITCODE -ne 0) { throw "gh release upload failed" }
    Write-Host "Uploaded to the data release."
}

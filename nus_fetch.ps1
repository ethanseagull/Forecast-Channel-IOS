param(
    [string]$TitleId = "0001000248414645",
    [int]$Version = 7,
    [Parameter(Mandatory)] [string]$OutDir
)
$ErrorActionPreference = "Stop"
$base = "http://nus.cdn.shop.wii.com/ccs/download/$TitleId"
$ua = "wii libnup/1.0"
New-Item -ItemType Directory -Force $OutDir | Out-Null

function Get-Bytes([string]$url) {
    $wc = New-Object Net.WebClient
    $wc.Headers.Add("User-Agent", $ua)
    return $wc.DownloadData($url)
}
function BE16([byte[]]$b, [int]$o) { return ([int]$b[$o] -shl 8) -bor $b[$o+1] }
function BE32([byte[]]$b, [int]$o) { return [uint32](([uint32]$b[$o] -shl 24) -bor ([uint32]$b[$o+1] -shl 16) -bor ([uint32]$b[$o+2] -shl 8) -bor $b[$o+3]) }
function Hex([byte[]]$b) { return -join ($b | ForEach-Object { $_.ToString("x2") }) }
function AesCbcDecrypt([byte[]]$key, [byte[]]$iv, [byte[]]$data) {
    $aes = [Security.Cryptography.Aes]::Create()
    $aes.Mode = "CBC"; $aes.Padding = "None"; $aes.Key = $key; $aes.IV = $iv
    $d = $aes.CreateDecryptor()
    return $d.TransformFinalBlock($data, 0, $data.Length)
}

$tmd = Get-Bytes "$base/tmd.$Version"
$cetk = Get-Bytes "$base/cetk"
[IO.File]::WriteAllBytes((Join-Path $OutDir "title.tmd"), $tmd)
[IO.File]::WriteAllBytes((Join-Path $OutDir "cetk"), $cetk)

# Wii common key (index 0): not in the repository. Supply it as 32 hex digits in
# $env:WII_COMMON_KEY or in common-key.txt next to this script (git-ignored).
$keyHex = $env:WII_COMMON_KEY
$keyFile = Join-Path $PSScriptRoot "common-key.txt"
if (-not $keyHex -and (Test-Path $keyFile)) { $keyHex = (Get-Content $keyFile -Raw) }
if (-not $keyHex) { throw "Set WII_COMMON_KEY or create $keyFile with the Wii common key (32 hex digits)." }
$keyHex = ($keyHex -replace '[^0-9A-Fa-f]', '')
if ($keyHex.Length -ne 32) { throw "The Wii common key must be 32 hex digits." }
$commonKey = [byte[]]::new(16)
for ($i = 0; $i -lt 16; $i++) { $commonKey[$i] = [Convert]::ToByte($keyHex.Substring($i * 2, 2), 16) }
$sha1 = [BitConverter]::ToString([Security.Cryptography.SHA1]::Create().ComputeHash($commonKey)).Replace("-", "").ToLower()
if ($sha1 -ne "ebeae6d2762d4d3ea160a6d8327fac9a25f8062b") { throw "That is not the Wii common key (SHA-1 mismatch)." }
if ($cetk[0x1F1] -ne 0) { throw "Ticket uses common key index $($cetk[0x1F1]), expected 0" }
$encTitleKey = $cetk[0x1BF..0x1CE]
$iv = New-Object byte[] 16; [Array]::Copy($cetk, 0x1DC, $iv, 0, 8)
$titleKey = AesCbcDecrypt $commonKey $iv $encTitleKey

$tmdVer = BE16 $tmd 0x1DC
$count = BE16 $tmd 0x1DE
"TMD version $tmdVer, $count contents, boot index $(BE16 $tmd 0x1E0)"
$sha1 = [Security.Cryptography.SHA1]::Create()
for ($i = 0; $i -lt $count; $i++) {
    $o = 0x1E4 + 36 * $i
    $cid = BE32 $tmd $o
    $idx = BE16 $tmd ($o + 4)
    $size = ([uint64](BE32 $tmd ($o + 8)) -shl 32) -bor [uint64](BE32 $tmd ($o + 12))
    $want = Hex $tmd[($o + 16)..($o + 35)]
    $cidHex = "{0:x8}" -f $cid
    $enc = Get-Bytes "$base/$cidHex"
    $civ = New-Object byte[] 16; $civ[0] = [byte]($idx -shr 8); $civ[1] = [byte]($idx -band 0xff)
    $dec = AesCbcDecrypt $titleKey $civ $enc
    $plain = New-Object byte[] $size; [Array]::Copy($dec, $plain, [int]$size)
    $got = Hex $sha1.ComputeHash($plain)
    $ok = if ($got -eq $want) { "OK" } else { "HASH MISMATCH" }
    [IO.File]::WriteAllBytes((Join-Path $OutDir ("{0:x8}.app" -f $idx)), $plain)
    "index {0,2}  cid {1}  size {2,8}  sha1 {3}  {4}" -f $idx, $cidHex, $size, $got, $ok
}

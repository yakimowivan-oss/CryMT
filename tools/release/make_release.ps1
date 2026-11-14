# Makes a CryMT release: builds, stages the package as it lies in the game folder, writes the signed manifest the
# launcher checks, zips, re-reads the zip against the manifest, and with -Publish puts it on GitHub (CryMT-Releases).
#   powershell -ExecutionPolicy Bypass -File tools\release\make_release.ps1 -Version 13 [-Test] [-Publish] [-NoBuild]
# Output: release\v<N>\out\ (CryMT-v<N>.zip, manifest.txt, manifest.sig, notes.md from tools\release\notes\v<N>.md).
# The signing key (keys\crymt-release.key) never leaves this machine; its public half is src\launcher\UpdateKey.h.
param([Parameter(Mandatory = $true)][int]$Version, [switch]$Test, [switch]$Publish, [switch]$NoBuild,
      [string]$Repo = 'yakimowivan-oss/CryMT-Releases', [string]$Key = '')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $Key) { $Key = "$root\keys\crymt-release.key" }
$build = "$root\build"
$dir = "$root\release\v$Version"
$stage = "$dir\staging"
$out = "$dir\out"
$sign = "$build\CryMTSign.exe"
$notes = "$PSScriptRoot\notes\v$Version.md"
if (-not (Test-Path $notes)) { throw "release notes missing: $notes" }
if (-not (Test-Path $Key)) { throw "signing key missing: $Key" }

if (-not $NoBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$root\build.ps1" -Out $build
    if ($LASTEXITCODE) { throw 'build failed' }
}
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
New-Item -ItemType Directory -Force $stage, $out | Out-Null

# Package layout = paths from the game folder. "keep" files are installed only when missing (the player's copy stays).
$files = [ordered]@{
    'Bin64/dbghelp.dll'                       = "$build\dbghelp.dll"
    'CryMT/CryMT.dll'                         = "$build\CryMT.dll"
    'CryMT/CryMTLauncher.exe'                 = "$build\CryMTLauncher.exe"
    'CryMT/CryMTSettings.exe'                 = "$build\CryMTSettings.exe"
    'CryMT/CryMTReport.exe'                   = "$build\CryMTReport.exe"
    'CryMT/WebView2Loader.dll'                = "$root\sdk\webview2\pkg\build\native\x64\WebView2Loader.dll"
    'CryMT/streamline/sl.interposer.dll'      = "$root\streamline\sl.interposer.dll"
    'CryMT/streamline/sl.common.dll'          = "$root\streamline\sl.common.dll"
    'CryMT/streamline/sl.dlss.dll'            = "$root\streamline\sl.dlss.dll"
    'CryMT/streamline/nvngx_dlss.dll'         = "$root\streamline\nvngx_dlss.dll"
    'CryMT/README_RU.txt'                     = "$PSScriptRoot\package\README_RU.txt"
    'CryMT/README_EN.txt'                     = "$PSScriptRoot\package\README_EN.txt"
    'CryMT/licenses/CryMT-GPL-3.0.txt'        = "$root\LICENSE"
    'CryMT/licenses/THIRD_PARTY.txt'          = "$PSScriptRoot\package\THIRD_PARTY.txt"
    'CryMT/licenses/nvngx_dlss.license.txt'   = "$root\streamline\nvngx_dlss.license.txt"
    'CryMT/licenses/streamline.license.txt'   = "$root\sdk\streamline-v2.14.16\license.txt"
    'CryMT/licenses/webview2.license.txt'     = "$root\sdk\webview2\pkg\LICENSE.txt"
    'CryMT/licenses/bend_sss.license.txt'     = "$root\src\crymt\BendSss.LICENSE.txt"
    'CryMT/licenses/fidelityfx.license.txt'   = "$root\src\crymt\FidelityFX.LICENSE.txt"
}
foreach ($ui in Get-ChildItem "$root\src\launcher\ui" -File) { $files["CryMT/ui/$($ui.Name)"] = $ui.FullName }
$keep = [ordered]@{ 'CryMT/cache/d3d12_psos.bin' = "$root\cache\d3d12_psos.bin" }

$lines = @("CryMT $Version", "date $(Get-Date -Format yyyy-MM-dd)", "channel $(if ($Test) { 'test' } else { 'stable' })")
foreach ($set in @(@{Kind = 'file'; Map = $files}, @{Kind = 'keep'; Map = $keep})) {
    foreach ($path in $set.Map.Keys) {
        $src = $set.Map[$path]
        if (-not (Test-Path $src)) { throw "missing: $src" }
        if ($path -match '\s') { throw "space in a package path: $path" }
        $dst = Join-Path $stage ($path -replace '/', '\')
        New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
        Copy-Item $src $dst
        $item = Get-Item $dst
        $lines += "$($set.Kind) $path $($item.Length) $((Get-FileHash $dst -Algorithm SHA256).Hash.ToLower())"
    }
}
$manifest = "$out\manifest.txt"
[IO.File]::WriteAllText($manifest, ($lines -join "`n") + "`n")
Copy-Item $manifest "$stage\CryMT\version.txt" # what a manual install leaves for the launcher
& $sign sign $Key $manifest "$out\manifest.sig" | Out-Null
& $sign verify $Key $manifest "$out\manifest.sig"
if ($LASTEXITCODE) { throw 'signature check failed' }

$zip = "$out\CryMT-v$Version.zip"
Push-Location $stage
& tar.exe -a -c -f $zip Bin64 CryMT
Pop-Location
if ($LASTEXITCODE) { throw 'zip failed' }

# Re-read: unpack the zip and check every file against the manifest, as the launcher will.
$check = "$dir\check"
New-Item -ItemType Directory -Force $check | Out-Null
& tar.exe -xf $zip -C $check
$bad = 0
foreach ($l in $lines | Where-Object { $_ -match '^(file|keep) ' }) {
    $p = $l.Split(' ')
    $f = Join-Path $check ($p[1] -replace '/', '\')
    if (-not (Test-Path $f) -or (Get-Item $f).Length -ne [int64]$p[2] -or (Get-FileHash $f -Algorithm SHA256).Hash.ToLower() -ne $p[3]) { "MISMATCH $($p[1])"; $bad++ }
}
Remove-Item $check -Recurse -Force
if ($bad) { throw "$bad files differ in the zip" }
Copy-Item $notes "$out\notes.md"
$count = @($lines | Where-Object { $_ -match '^(file|keep) ' }).Count
"v$Version ($(if ($Test) { 'test' } else { 'stable' })): $count files, zip $([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB, all files re-read from the zip OK"

if ($Publish) {
    $ghArgs = @('release', 'create', "v$Version", '--repo', $Repo, '--title', "CryMT v$Version", '--notes-file', "$out\notes.md", $zip, $manifest, "$out\manifest.sig")
    if ($Test) { $ghArgs += '--prerelease' }
    & gh @ghArgs
    if ($LASTEXITCODE) { throw 'gh release create failed' }
}

# Turns CryMT's screen-space GI on or off for play tests until CryMT installs its shader patch itself.
# -On: patched shaders and the placeholder as loose files in Game, GI=1 in CryMT.ini (D3D12 modes: Crysis_MT_DX12*.cmd).
# -Off: removes them, GI=0, and returns the user shader cache to the backup taken before the first shader test.
param([switch]$On, [switch]$Off)
$game = 'D:\Games\Crysis\Game'
$patched = 'D:\Games\Crysis-RE\src\crymt\shaders\patched'
$ini = 'D:\Games\Crysis\CryMT\CryMT.ini'
$cache = "$env:USERPROFILE\Documents\My Games\Crysis\Shaders"
$backup = 'D:\Games\Crysis-RE\backup\usershaders_before_gitest2'
if (Get-Process Crysis -ErrorAction SilentlyContinue) { 'Close Crysis first'; exit 1 }
$text = (Get-Content $ini) | Where-Object { $_ -notmatch '^GI=' }
if ($On) {
    foreach ($dir in 'Shaders', 'Textures') { Copy-Item (Join-Path $patched $dir) $game -Recurse -Force }
    $text += 'GI=1'
    Set-Content $ini $text -Encoding Ascii
    'GI on: start Crysis_MT_DX12.cmd or Crysis_MT_DX12_DLAA.cmd (the first start compiles shaders for a while)'
}
elseif ($Off) {
    Remove-Item (Join-Path $game 'Shaders'), (Join-Path $game 'Textures') -Recurse -Force -ErrorAction SilentlyContinue
    Set-Content $ini $text -Encoding Ascii
    Get-ChildItem $cache -Recurse -File | ForEach-Object {
        $orig = Join-Path $backup $_.FullName.Substring($cache.Length + 1)
        if (-not (Test-Path $orig)) { Remove-Item $_.FullName -Force }
        elseif ((Get-FileHash $orig).Hash -ne (Get-FileHash $_.FullName).Hash) { Copy-Item $orig $_.FullName -Force }
    }
    'GI off: shaders removed, shader cache restored'
}
else { 'use -On or -Off' }

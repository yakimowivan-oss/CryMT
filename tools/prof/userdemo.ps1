# The user's own setup (CryMT.ini as the dbghelp proxy applies it: dlaa mode, [Env], [CVars]) with overrides, on a
# timedemo, with the every-frame stream capture (CRYMT_CAPTURE_STREAM=4), then counts the frames whose picture changes
# by more than -Threshold pixels (of 1280x360) from the frame before after world frame -From (a static camera: the
# flicker). Usage: userdemo.ps1 -Tag name [-Override @{CRYMT_X='0'}] [-Level mine] [-Script giplay_rooms]
# [-Extra @('+r_TexturesMipBiasing', '1')] (command line before the map: cvars read at load)
param([string]$Tag = 'base', [hashtable]$Override = @{}, [string]$Level = 'mine', [string]$Script = 'giplay_rooms', [int]$From = 35,
      [int]$Threshold = 500, [int]$MaxRunSec = 240, [string[]]$Extra = @())
$root = 'D:\Games\Crysis'
$ini = "$root\CryMT\CryMT.ini"
$vars = [ordered]@{CRYMT_MODE='2'; CRYMT_LEVEL='2'; CRYMT_FILL='1'; CRYMT_TERRAINOWN='1'; CRYMT_D3D12='1'; CRYMT_D3DSTREAM='1'; CRYMT_DLSS='1';
                   CRYMT_BORDERLESS='1'; CRYMT_PAUSEONFOCUS='0'}
$section = ''
$cvars = @('r_DynTexMaxSize 256', 'r_UseEdgeAA 0')
foreach ($line in Get-Content $ini) {
    $t = $line.Trim()
    if ($t -match '^\[(.+)\]$') { $section = $Matches[1]; continue }
    if ($t -eq '' -or $t.StartsWith(';') -or -not $t.Contains('=')) { continue }
    $k, $v = $t.Split('=', 2)
    if ($section -eq 'Env') { $vars[$k.Trim()] = $v.Trim() }
    elseif ($section -eq 'CVars') { $cvars += "$($k.Trim()) $($v.Trim())" }
    elseif ($section -eq 'CryMT' -and $k.Trim() -eq 'GI' -and $v.Trim() -ne '0') { $vars['CRYMT_GI'] = $v.Trim() }
}
$vars['CRYMT_TESTCMD'] = $cvars -join ';'
$vars['CRYMT_TESTCMDSEC'] = '0'
foreach ($k in $Override.Keys) { $vars[$k] = $Override[$k] }
$vars['CRYMT_CAPTURE_STREAM'] = '4'
$vars['CRYMT_CAPTURE_TAG'] = "ud_$Tag"
$raw = "$root\CryMT\logs\play\ud_${Tag}_stream.raw"
Remove-Item $raw -ErrorAction SilentlyContinue
foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }
$id = & "$root\CryMT\Inject.exe" "$root\CryMT\CryMT.dll" $root -dx10 -DEVMODE @Extra +map $Level +exec $Script
foreach ($k in $vars.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
$p = Get-Process -Id ([int]$id) -ErrorAction SilentlyContinue
$t0 = Get-Date
while ($p -and -not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt $MaxRunSec) { Start-Sleep -Seconds 2 }
if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force; 'force-stopped' }
python "D:\Games\Crysis-RE\tools\prof\streamspikes.py" $raw $From $Threshold $Tag

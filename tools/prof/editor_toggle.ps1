# Starts the Sandbox Editor with CryMT injected and sends Ctrl+G (game mode on/off) at the given seconds, then closes it.
# It does NOT open a level: the editor starts empty. In the 2026-11-13 runs the user opened fleet by hand and went into
# the game inside a VisArea (the ship) before the toggles. Logs go to D:\Games\Crysis-RE\logs\editor\<tag>.*
param([string]$Tag = 'etog', [string]$FrameIdSync = '1', [int]$Seconds = 100, [string]$Toggles = '25,35,42,50,57,64', [string]$Extra = '')
$g = 'D:\Games\Crysis'
$out = 'D:\Games\Crysis-RE\logs\editor'
New-Item -ItemType Directory -Force $out | Out-Null
$logs = "$g\CryMT\logs\mt"
Remove-Item "$logs\crymt_fault.txt" -ErrorAction SilentlyContinue
$env:CRYMT_MODE = '2'; $env:CRYMT_LEVEL = '2'; $env:CRYMT_FILL = '1'; $env:CRYMT_TERRAINOWN = '1'
$env:CRYMT_D3D12 = '1'; $env:CRYMT_D3DSTREAM = '1'; $env:CRYMT_DLSS = '1'; $env:CRYMT_GI = '1'
$env:CRYMT_PAUSEONFOCUS = '0'; $env:CRYMT_ASYNCSAVE = '0'; $env:CRYMT_BORDERLESS = '0'
$env:CRYMT_FRAMEIDSYNC = $FrameIdSync
$extraNames = @()
foreach ($kv in ($Extra.Split(';') | Where-Object { $_ -match '=' })) {
    $n, $v = $kv.Split('=', 2); Set-Item "env:$n" $v; $extraNames += $n; "env $n=$v"
}
$editorPid = & "$g\CryMT\Inject.exe" --exe EditorLauncher.exe "$g\CryMT\CryMT.dll" $g
foreach ($k in @('CRYMT_MODE','CRYMT_LEVEL','CRYMT_FILL','CRYMT_TERRAINOWN','CRYMT_D3D12','CRYMT_D3DSTREAM','CRYMT_DLSS','CRYMT_GI','CRYMT_PAUSEONFOCUS','CRYMT_ASYNCSAVE','CRYMT_BORDERLESS','CRYMT_FRAMEIDSYNC') + $extraNames) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
"editor pid: $editorPid"
Add-Type -AssemblyName System.Windows.Forms, System.Drawing, Microsoft.VisualBasic
$t0 = Get-Date
$at = @($Toggles.Split(',') | ForEach-Object { [int]$_ })
$done = 0
$exited = $false
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    Start-Sleep -Milliseconds 500
    $p = Get-Process -Id $editorPid -ErrorAction SilentlyContinue
    if (-not $p) { "editor exited at $([int]((Get-Date) - $t0).TotalSeconds) s"; $exited = $true; break }
    if ($done -lt $at.Count -and ((Get-Date) - $t0).TotalSeconds -ge $at[$done]) {
        try { [Microsoft.VisualBasic.Interaction]::AppActivate([int]$editorPid) } catch { "activate failed: $_" }
        Start-Sleep -Milliseconds 300
        [System.Windows.Forms.SendKeys]::SendWait('^g')
        "ctrl+g $($done + 1) at $([int]((Get-Date) - $t0).TotalSeconds) s"
        $done++
    }
}
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
[System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
$bmp.Save("$out\$Tag.screen.png", [System.Drawing.Imaging.ImageFormat]::Png)
if (-not $exited) { Stop-Process -Id $editorPid -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 3
foreach ($f in 'crymt_stats.txt', 'crymt_fault.txt', 'crymt_focus.txt') {
    if (Test-Path "$logs\$f") { Copy-Item "$logs\$f" "$out\$Tag.$f" -Force }
}
Copy-Item "$g\Editor.log" "$out\$Tag.Editor.log" -Force

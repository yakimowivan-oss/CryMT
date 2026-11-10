# Starts the Sandbox Editor (c1-launcher's Bin64\EditorLauncher.exe) with CryMT injected and a level open, captures the
# back buffer (CRYMT_CAPTURE_SEC) and the screen after some seconds, then closes it. Logs go to logs\editor\<tag>.*.
#   powershell -File tools\prof\editor_test.ps1 -Tag ed1 [-Mode 2] [-Level village] [-Seconds 110] [-Dlss 1] [-Gi 1]
param([string]$Tag = 'ed1', [string]$Mode = '2', [string]$Level = 'village', [int]$Seconds = 110, [string]$Dlss = '1', [string]$Gi = '1',
      [string]$Shots = '80,105')
$g = 'D:\Games\Crysis'
$out = 'D:\Games\Crysis-RE\logs\editor'
New-Item -ItemType Directory -Force $out | Out-Null
$logs = "$g\CryMT\logs\mt"
$env:CRYMT_MODE = $Mode; $env:CRYMT_LEVEL = '2'; $env:CRYMT_FILL = '1'; $env:CRYMT_TERRAINOWN = '1'
$env:CRYMT_D3D12 = '1'; $env:CRYMT_D3DSTREAM = '1'
if ($Dlss -ne '0') { $env:CRYMT_DLSS = '1' }
if ($Gi -ne '0') { $env:CRYMT_GI = '1' }
$env:CRYMT_PAUSEONFOCUS = '0'; $env:CRYMT_ASYNCSAVE = '0'; $env:CRYMT_BORDERLESS = '0'
$env:CRYMT_CAPTURE_SEC = $Shots; $env:CRYMT_CAPTURE_COUNT = '1'; $env:CRYMT_CAPTURE_TAG = $Tag
Remove-Item "$g\CryMT\logs\play\${Tag}_*.ppm" -ErrorAction SilentlyContinue
$cry = "$g\Game\Levels\$Level\$Level.cry"
$editorPid = & "$g\CryMT\Inject.exe" --exe EditorLauncher.exe "$g\CryMT\CryMT.dll" $g "`"$cry`""
foreach ($k in 'CRYMT_MODE','CRYMT_LEVEL','CRYMT_FILL','CRYMT_TERRAINOWN','CRYMT_D3D12','CRYMT_D3DSTREAM','CRYMT_DLSS','CRYMT_GI','CRYMT_PAUSEONFOCUS','CRYMT_ASYNCSAVE','CRYMT_BORDERLESS','CRYMT_CAPTURE_SEC','CRYMT_CAPTURE_COUNT','CRYMT_CAPTURE_TAG') { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
"editor pid: $editorPid"
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$t0 = Get-Date
$shotAt = @($Shots.Split(',') | ForEach-Object { [int]$_ + 3 })
$taken = 0
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    Start-Sleep -Seconds 1
    $p = Get-Process -Id $editorPid -ErrorAction SilentlyContinue
    if (-not $p) { 'editor exited early'; break }
    if ($taken -lt $shotAt.Count -and ((Get-Date) - $t0).TotalSeconds -ge $shotAt[$taken]) {
        $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
        [System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
        $bmp.Save("$out\$Tag.screen$taken.png", [System.Drawing.Imaging.ImageFormat]::Png)
        $taken++
    }
}
Stop-Process -Id $editorPid -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
foreach ($f in 'crymt_stats.txt', 'd3d12.txt', 'crymt_fault.txt', 'd3dlayer.txt', 'crymt_off.txt', 'd3d10_census.txt') {
    if (Test-Path "$logs\$f") { Copy-Item "$logs\$f" "$out\$Tag.$f" -Force }
}
Get-ChildItem "$g\CryMT\logs\play\${Tag}_*.ppm" -ErrorAction SilentlyContinue | ForEach-Object { Copy-Item $_.FullName $out -Force; $_.Name }

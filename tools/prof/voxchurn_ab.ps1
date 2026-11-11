# Voxel churn on a still camera for several CryMT variants: per variant a still pair in village (still_pair.ps1:
# F2 twice, two PgUp RenderDoc captures 35 s apart), then rdc_voxdiff.py on the pair. Results in logs\voxchurn\<tag>.txt.
#   powershell -File tools\prof\voxchurn_ab.ps1 -Variants "cur;old:CRYMT_ASYNCQUERIES=0,CRYMT_DIRECTMAPS=0"
param([string]$Variants = 'cur')
$out = 'D:\Games\Crysis-RE\logs\voxchurn'
New-Item -ItemType Directory -Force $out | Out-Null
$dir = 'D:\Games\Crysis\CryMT\logs\renderdoc'
foreach ($v in $Variants.Split(';')) {
    $parts = $v.Split(':', 2)
    $tag = $parts[0]
    $envs = @()
    if ($parts.Count -gt 1 -and $parts[1]) { $envs = $parts[1].Split(',') }
    foreach ($e in $envs) { $kv = $e.Split('=', 2); Set-Item "env:$($kv[0])" $kv[1] }
    $before = @(Get-ChildItem "$dir\*.rdc" | ForEach-Object { $_.FullName })
    & powershell -ExecutionPolicy Bypass -File D:\Games\Crysis-RE\tools\prof\still_pair.ps1 -Mode mixed | Out-Null
    foreach ($e in $envs) { Remove-Item "env:$($e.Split('=', 2)[0])" -ErrorAction SilentlyContinue }
    $new = @(Get-ChildItem "$dir\*.rdc" | Where-Object { $before -notcontains $_.FullName } | Sort-Object LastWriteTime)
    if ($new.Count -lt 2) { "$tag : only $($new.Count) captures" | Out-File "$out\$tag.txt"; continue }
    $env:RDC_FILE = $new[0].FullName; $env:RDC_FILE2 = $new[1].FullName; $env:RDC_OUT = "$out\$tag.txt"
    $p = Start-Process 'D:\Games\Crysis\CryMT\renderdoc\qrenderdoc.exe' -ArgumentList '--python', 'D:\Games\Crysis-RE\tools\prof\rdc_voxdiff.py' -PassThru -WindowStyle Minimized
    if (-not $p.WaitForExit(600000)) { Stop-Process -Id $p.Id -Force }
    Remove-Item Env:RDC_FILE, Env:RDC_FILE2, Env:RDC_OUT
    Add-Content "$out\$tag.txt" ("captures: " + ($new | ForEach-Object { $_.Name }) -join ' ')
}
'done'

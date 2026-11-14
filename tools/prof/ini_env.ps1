# Sets (or comments out with -Value '') one [Env] key in the user's CryMT.ini, keeping the file's bytes otherwise
# (UTF-8 without BOM, its line endings): for test runs, since the ini overrides the test scripts' environment.
#   powershell -File tools\prof\ini_env.ps1 -Key CRYMT_GI_VOXELAO -Value 0.6     prints the previous value
param([string]$Key, [string]$Value, [string]$Ini = 'D:\Games\Crysis\CryMT\CryMT.ini')
$enc = New-Object System.Text.UTF8Encoding($false)
$text = [IO.File]::ReadAllText($Ini, $enc)
$m = [regex]::Match($text, "(?m)^$([regex]::Escape($Key))=([^\r\n]*)")
$old = if ($m.Success) { $m.Groups[1].Value } else { '' }
if ($m.Success) {
    $text = $text.Substring(0, $m.Index) + "$Key=$Value" + $text.Substring($m.Index + $m.Length)
} else {
    $text = [regex]::Replace($text, "(?m)^\[Env\]\r?$", "[Env]`r`n$Key=$Value", 1)
}
[IO.File]::WriteAllText($Ini, $text, $enc)
"$Key was '$old', now '$Value'"

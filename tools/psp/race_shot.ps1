# Runs the PSP race (build/psp/tm_race/EBOOT.PBP) in PPSSPP in auto mode and checks it against the host.
#   powershell -ExecutionPolicy Bypass -File tools\psp\race_shot.ps1 [-Track easy/0] [-Frames 150] [-Out docs\psp_race.png]
# Installs the EBOOT and extract/files in PPSSPP's memstick (PSP/GAME/TM2PSP, never a real PSP), writes auto.txt,
# waits for race_log.txt to end with the memory line, converts shot.bmp to PNG, and compares the "frame"/"camera"
# lines with `tm_race --frames N --auto` on the host (same game code: they must be identical).
# -Screen menu (with -Track menu) / pause / result: captures that menu instead (no comparison with the host).
param([string]$Track = 'easy/0', [int]$Frames = 150, [string]$Out = 'build\psp\race_shot.png', [int]$Timeout = 300,
      [string]$Screen = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $repo
$g = 'C:\Tools\PPSSPP\memstick\PSP\GAME\TM2PSP'
New-Item -ItemType Directory -Force $g | Out-Null
Copy-Item build\psp\tm_race\EBOOT.PBP "$g\EBOOT.PBP" -Force
robocopy extract\files "$g\files" /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
Remove-Item "$g\shot.bmp", "$g\race_log.txt" -ErrorAction SilentlyContinue
"$Track $Frames $Screen".Trim() | Out-File -Encoding ascii "$g\auto.txt"
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process 'C:\Tools\PPSSPP\PPSSPPWindows64.exe' -ArgumentList "`"$g\EBOOT.PBP`"" -PassThru
while ($sw.Elapsed.TotalSeconds -lt $Timeout -and -not $p.HasExited) {
    if ((Test-Path "$g\race_log.txt") -and (Select-String -Path "$g\race_log.txt" -Pattern '^memory' -Quiet)) { break }
    Start-Sleep -Seconds 1
}
Start-Sleep -Seconds 1
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -Confirm:$false }
Remove-Item "$g\auto.txt"
if (-not (Test-Path "$g\race_log.txt")) { Write-Output "no race_log.txt"; exit 1 }
Copy-Item "$g\race_log.txt" build\psp\race_log.txt -Force
Get-Content build\psp\race_log.txt
if (Test-Path "$g\shot.bmp") {
    python -P -c "from PIL import Image; Image.open(r'$g\shot.bmp').save(r'$Out')"
    Write-Output "screenshot: $Out"
}
if ($Screen) { exit 0 }
$host_lines = & build\host\tm_race.exe --root extract/files --track $Track --frames $Frames --auto --shot build\host\race_ref.bmp |
    Where-Object { $_ -match '^(frame|camera) ' }
$psp_lines = Get-Content build\psp\race_log.txt | Where-Object { $_ -match '^(frame|camera) ' }
if (($host_lines -join "`n") -eq ($psp_lines -join "`n") -and $psp_lines.Count -eq 2) {
    Write-Output "car and camera after $Frames frames: identical to the host"
} else {
    Write-Output "DIFFERENT from the host:"; $host_lines; exit 1
}

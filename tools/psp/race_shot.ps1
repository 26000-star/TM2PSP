# Runs the PSP race (build/psp/tm_race/EBOOT.PBP) in PPSSPP in auto mode and checks it against the host.
#   powershell -ExecutionPolicy Bypass -File tools\psp\race_shot.ps1 [-Track easy/0] [-Frames 150] [-Out docs\psp_race.png]
# Installs the EBOOT and extract/files in PPSSPP's memstick (PSP/GAME/TM2PSP, never a real PSP), writes auto.txt,
# waits for race_log.txt to end with the sound line, converts shot.bmp to PNG, and compares the "frame"/"camera"
# lines and the hash of the rendered sound with `tm_race --frames N --auto --wav` on the host (same game code and
# sound driver: they must be identical). -Wav also copies the PSP's sound to build\psp\race.wav.
# -Screen menu (with -Track menu) / pause / result: captures that menu instead (no comparison with the host);
# -Screen missed / freewheel: a HUD message 10 frames before the shot (no comparison either).
param([string]$Track = 'easy/0', [int]$Frames = 150, [string]$Out = 'build\psp\race_shot.png', [int]$Timeout = 300,
      [string]$Screen = '', [switch]$Wav)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $repo
$g = 'C:\Tools\PPSSPP\memstick\PSP\GAME\TM2PSP'
New-Item -ItemType Directory -Force $g | Out-Null
Copy-Item build\psp\tm_race\EBOOT.PBP "$g\EBOOT.PBP" -Force
robocopy extract\files "$g\files" /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
# The front end's pictures (tools/fe/convert.py, converted again when missing).
if (-not (Test-Path build\fe\data\background.fet)) { python -P tools\fe\convert.py | Out-Null }
robocopy build\fe\data "$g\fe" /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
Remove-Item "$g\shot.bmp", "$g\race_log.txt", "$g\race.wav", "$g\wav.txt" -ErrorAction SilentlyContinue
if ($Wav) { '' | Out-File -Encoding ascii "$g\wav.txt" }
"$Track $Frames $Screen".Trim() | Out-File -Encoding ascii "$g\auto.txt"
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process 'C:\Tools\PPSSPP\PPSSPPWindows64.exe' -ArgumentList "`"$g\EBOOT.PBP`"" -PassThru
while ($sw.Elapsed.TotalSeconds -lt $Timeout -and -not $p.HasExited) {
    if ((Test-Path "$g\race_log.txt") -and (Select-String -Path "$g\race_log.txt" -Pattern '^sound' -Quiet)) { break }
    Start-Sleep -Seconds 1
}
Start-Sleep -Seconds 1
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -Confirm:$false }
Remove-Item "$g\auto.txt"
Remove-Item "$g\wav.txt" -ErrorAction SilentlyContinue
if ($Wav -and (Test-Path "$g\race.wav")) { Copy-Item "$g\race.wav" build\psp\race.wav -Force }
if (-not (Test-Path "$g\race_log.txt")) { Write-Output "no race_log.txt"; exit 1 }
Copy-Item "$g\race_log.txt" build\psp\race_log.txt -Force
Get-Content build\psp\race_log.txt
if (Test-Path "$g\shot.bmp") {
    python -P -c "from PIL import Image; Image.open(r'$g\shot.bmp').save(r'$Out')"
    Write-Output "screenshot: $Out"
}
if ($Screen) { exit 0 }
$host_lines = & build\host\tm_race.exe --root extract/files --track $Track --frames $Frames --auto --shot build\host\race_ref.bmp --wav build\host\race_ref.wav |
    Where-Object { $_ -match '^(frame|camera|sound) ' }
$psp_lines = Get-Content build\psp\race_log.txt | Where-Object { $_ -match '^(frame|camera|sound) ' }
if (($host_lines -join "`n") -eq ($psp_lines -join "`n") -and $psp_lines.Count -eq 3) {
    Write-Output "car, camera and sound after $Frames frames: identical to the host"
} else {
    Write-Output "DIFFERENT from the host:"; $host_lines; exit 1
}

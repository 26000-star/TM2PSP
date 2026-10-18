# Installs an EBOOT (plus optional data files) into PPSSPP's memstick, runs it until the program writes
# shot.bmp, closes PPSSPP and converts the screenshot to PNG.
# Usage: powershell -File tools\ppsspp_shot.ps1 <EBOOT.PBP> <game folder name> <out.png> [<src>=<dest name> ...]
param([string]$Eboot, [string]$Folder, [string]$Out)
$ErrorActionPreference = 'Stop'
$g = "C:\Tools\PPSSPP\memstick\PSP\GAME\$Folder"
New-Item -ItemType Directory -Force $g | Out-Null
Copy-Item $Eboot "$g\EBOOT.PBP" -Force
foreach ($pair in $args) { $s, $d = $pair -split '=', 2; Copy-Item $s "$g\$d" -Force }
Remove-Item "$g\shot.bmp" -ErrorAction SilentlyContinue
$p = Start-Process 'C:\Tools\PPSSPP\PPSSPPWindows64.exe' -ArgumentList "`"$g\EBOOT.PBP`"" -PassThru
$t = 0
while (-not (Test-Path "$g\shot.bmp") -and $t -lt 60) { Start-Sleep -Milliseconds 500; $t++ }
Start-Sleep -Seconds 1
Stop-Process -Id $p.Id -Force -Confirm:$false
if (-not (Test-Path "$g\shot.bmp")) { Write-Output "no screenshot"; exit 1 }
python -P -c "from PIL import Image; Image.open(r'$g\shot.bmp').save(r'$Out')"
Write-Output "screenshot: $Out"

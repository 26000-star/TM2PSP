# Reload test of the PSP race (build/psp/tm_race/EBOOT.PBP) in PPSSPP: loads the Coast tracks one after the other
# (menu order, 20 frames each, -Count loads in all) and checks that the heap, the back-end texture and vertex bytes
# and the files open through the FS shim come back to the same values after every unload.
#   powershell -ExecutionPolicy Bypass -File tools\psp\reload_test.ps1 [-Count 32]
# Installs the EBOOT and extract/files in PPSSPP's memstick (PSP/GAME/TM2PSP, never a real PSP) and writes reload.txt.
param([int]$Count = 32, [int]$Timeout = 600)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $repo
$g = 'C:\Tools\PPSSPP\memstick\PSP\GAME\TM2PSP'
New-Item -ItemType Directory -Force $g | Out-Null
Copy-Item build\psp\tm_race\EBOOT.PBP "$g\EBOOT.PBP" -Force
robocopy extract\files "$g\files" /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
Remove-Item "$g\race_log.txt", "$g\auto.txt" -ErrorAction SilentlyContinue
"$Count" | Out-File -Encoding ascii "$g\reload.txt"
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process 'C:\Tools\PPSSPP\PPSSPPWindows64.exe' -ArgumentList "`"$g\EBOOT.PBP`"" -PassThru
while ($sw.Elapsed.TotalSeconds -lt $Timeout -and -not $p.HasExited) {
    if ((Test-Path "$g\race_log.txt") -and (Select-String -Path "$g\race_log.txt" -Pattern '^reload test done' -Quiet)) { break }
    Start-Sleep -Seconds 1
}
Start-Sleep -Seconds 1
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -Confirm:$false }
Remove-Item "$g\reload.txt"
if (-not (Test-Path "$g\race_log.txt")) { Write-Output "no race_log.txt"; exit 1 }
Copy-Item "$g\race_log.txt" build\psp\reload_log.txt -Force
$lines = Get-Content build\psp\reload_log.txt | Where-Object { $_ -match '^reload \d+ ' }
$lines | Select-Object -First 3
$lines | Select-Object -Last 2
# Before the first load: the singletons that stay loaded (piece table, car parameters...) are not allocated yet,
# so only the open files (the VFS archive) are compared with it.
$start = (Get-Content build/psp/reload_log.txt | Where-Object { $_ -match '^start ' }) -replace '^start ', ''
$startFiles = ($start -split ' ') | Where-Object { $_ -match '^files=' }
# Warm-up: a container kept by the game singletons grows once (512 bytes at the third load); after that the heap
# must not move across the remaining loads (every track at least once for -Count >= 29).
$values = $lines | Select-Object -Skip 3 | ForEach-Object { ($_ -split ' ', 3)[2] } | Sort-Object -Unique
if ($lines.Count -ge $Count -and $values.Count -eq 1 -and $values -match "textures=0 meshes=0 $startFiles`$") {
    Write-Output "$($lines.Count) reloads: heap, textures, meshes and open files identical after every unload from the 4th on ($values; before the first load: $start)"
} else {
    Write-Output "LEAK or incomplete run: $($lines.Count) reloads, distinct states:"; $values; exit 1
}

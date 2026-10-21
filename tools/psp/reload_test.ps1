# Reload test of the PSP race (build/psp/tm_race/EBOOT.PBP) in PPSSPP: loads the tracks one after the other (menu
# order: the 26 of Stadium, then Island, Snow and Coast, then the 17 platform tracks, pla/<difficulty>/<n> being
# environment n; 20 frames each, -Count loads in all) and checks that the heap, the back-end texture (and VRAM) and
# vertex bytes and the files open through the FS shim come back to the same values after every unload of an
# environment's tracks (the singletons kept between races, such as the piece catalogue, have a size per environment;
# some of their containers keep the capacity of the largest track seen, a few hundred bytes the first time). The
# environment of each unload comes from the log's "track <id>:" line. -Count 242 or more: two rounds of the 121
# tracks, and every environment must come back to one state in the second round; fewer: within an environment,
# after its 3rd load, the heap may only step up, by 1 KB in all (capacity), and textures, meshes and files must be
# back to none.
#   powershell -ExecutionPolicy Bypass -File tools\psp\reload_test.ps1 [-Count 32 | -Count 242 -Timeout 3600]
# Installs the EBOOT and extract/files in PPSSPP's memstick (PSP/GAME/TM2PSP, never a real PSP) and writes reload.txt.
param([int]$Count = 32, [int]$Timeout = 600)
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
# Each unload with the track it unloaded; the round is how many times the first track came back.
$entries = @()
$track = ''
foreach ($l in Get-Content build\psp\reload_log.txt) {
    if ($l -match '^track (\S+): ') {
        $track = $Matches[1]
    } elseif ($l -match '^reload \d+ (.*)$') {
        $state = $Matches[1]
        $envIndex = if ($track -match '^pla/[^/]+/(\d+)') { [int]$Matches[1] } else { [array]::IndexOf(@('sta', 'isl', 'sno', 'coa'), $track.Substring(0, 3)) }
        $entries += [pscustomobject]@{ track = $track; env = $envIndex; state = $state }
    }
}
$roundLen = $entries.Count
for ($i = 1; $i -lt $entries.Count; $i++) { if ($entries[$i].track -eq $entries[0].track) { $roundLen = $i; break } }
$ok = $entries.Count -ge $Count
$strict = $roundLen -lt $entries.Count -and $entries.Count -ge 2 * $roundLen
$report = @()
foreach ($round in 0..[math]::Floor(($entries.Count - 1) / $roundLen)) {
    $inRound = @($entries | Select-Object -Skip ($round * $roundLen) -First $roundLen)
    foreach ($e in 0..3) {
        $group = @($inRound | Where-Object { $_.env -eq $e })
        if ($group.Count -eq 0) { continue }
        $skip = if ($strict) { 0 } else { 3 }
        $states = @($group | Select-Object -Skip $skip | ForEach-Object { $_.state })
        $values = @($states | Sort-Object -Unique)
        foreach ($v in $values) {
            if ($v -notmatch "textures=0 vram=0 meshes=0 $startFiles`$") { $ok = $false }
        }
        $heaps = @($states | ForEach-Object { [int](($_ -split ' ')[0] -replace 'heap=', '') })
        if ($strict -and $round -ge 1 -and $values.Count -gt 1) { $ok = $false }
        if (-not $strict -and $heaps.Count -gt 0) {
            for ($i = 1; $i -lt $heaps.Count; $i++) { if ($heaps[$i] -lt $heaps[$i - 1]) { $ok = $false } }
            if ($heaps[-1] - $heaps[0] -gt 1024) { $ok = $false }
        }
        if (-not $strict -or $round -ge 1) {
            $report += "round $round environment $e ($($group.Count) loads): $($values -join ' | ')"
        }
    }
}
if ($ok) {
    Write-Output "$($entries.Count) reloads: no leak (heap, textures, meshes and open files; before the first load: $start)"
    $report
} else {
    Write-Output "LEAK or incomplete run: $($entries.Count) reloads, distinct states:"; $report; exit 1
}

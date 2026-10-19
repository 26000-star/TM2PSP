# Runs the PSP check suite (build/psp/tm_checks/EBOOT.PBP) in PPSSPP and compares it with the host.
#   powershell -File tools\psp\run_checks_psp.ps1 [-Timeout 1800] [-Quick]
# 1. host reference: build/host/tm_checksuite.exe over the same list -> build/host/suite
# 2. installs the EBOOT, the list and extract/files (robocopy, only changed files) in PPSSPP's memstick under
#    PSP/GAME/TM2PSPCHK, starts PPSSPP, waits for out/suite.log, closes PPSSPP, copies out/ to build/psp/suite
# 3. byte-for-byte comparison (tools/psp/compare_checks.py) and the host checkers on the PSP dumps.
# Only PPSSPP's memstick folder is written: nothing goes to a real PSP.
param([int]$Timeout = 1800, [switch]$Quick)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $repo
$g = 'C:\Tools\PPSSPP\memstick\PSP\GAME\TM2PSPCHK'
New-Item -ItemType Directory -Force $g, 'build\host\suite', 'build\psp\suite' | Out-Null

python -P tools/host/run_checks.py --list | Out-File -Encoding ascii build\host\checks.txt
if ($Quick) {
    (Get-Content build\host\checks.txt) | Where-Object { $_ -match '^(carparams|drop|track_easy_0|progress_easy_0|camera_easy_0) ' } |
        Out-File -Encoding ascii build\host\checks.txt
}
& build\host\tm_checksuite.exe --root extract/files --list build/host/checks.txt --out build/host/suite
if ($LASTEXITCODE -ne 0) { Write-Output "host suite failed"; exit 1 }

Copy-Item build\psp\tm_checks\EBOOT.PBP "$g\EBOOT.PBP" -Force
Copy-Item build\host\checks.txt "$g\checks.txt" -Force
robocopy extract\files "$g\files" /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
if (Test-Path "$g\out") { Remove-Item -Recurse -Force "$g\out" }

$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process 'C:\Tools\PPSSPP\PPSSPPWindows64.exe' -ArgumentList "`"$g\EBOOT.PBP`"" -PassThru
while (-not (Test-Path "$g\out\suite.log") -and $sw.Elapsed.TotalSeconds -lt $Timeout -and -not $p.HasExited) {
    Start-Sleep -Seconds 2
}
Start-Sleep -Seconds 1
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -Confirm:$false }
Write-Output ("PPSSPP wall time: {0:N0} s" -f $sw.Elapsed.TotalSeconds)
Remove-Item -Recurse -Force build\psp\suite
if (-not (Test-Path "$g\out")) { Write-Output "no output"; exit 1 }
Copy-Item -Recurse "$g\out" build\psp\suite
if (-not (Test-Path build\psp\suite\suite.log)) { Write-Output "the suite did not finish (no suite.log)"; exit 1 }
python -P tools/psp/compare_checks.py build/host/suite build/psp/suite build/host/checks.txt
$cmp = $LASTEXITCODE
if ($Quick) { exit $cmp }
python -P tools/host/run_checks.py --dumps build/psp/suite | Select-Object -Last 1
if ($cmp -ne 0 -or $LASTEXITCODE -ne 0) { exit 1 }

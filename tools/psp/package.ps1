# Prepares the memory stick folder of the PSP game in build/psp/package (not in git: it holds the game files
# extracted from your own ROM) from extract/files and the EBOOT of tools/psp_build.sh. The work is done by
# builder.py (the same packaging as a build from the ROM). Copy build\psp\package\PSP to the root of the memory stick
# yourself; this script never writes to a PSP.
#   powershell -ExecutionPolicy Bypass -File tools\psp\package.ps1 [-Perf] [-Diag]
# -Perf adds an empty perf.txt: the race then appends its per-second frame times to frametimes.txt.
# -Diag adds diag.txt: at the first launch the race saves diag_<view>_<variant>.bmp (fixed views under bridges, one
# image per rendering variant: see RunDiag in src/psp/race_psp.cpp), deletes diag.txt and goes on to the menu.
param([switch]$Perf, [switch]$Diag)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $repo
$builderArgs = @('-P', 'builder.py', '--files', 'extract\files', '--eboot', 'build\psp\tm_race\EBOOT.PBP',
                 '--out', 'build\psp\package')
if ($Perf) { $builderArgs += '--perf' }
if ($Diag) { $builderArgs += '--diag' }
& python @builderArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

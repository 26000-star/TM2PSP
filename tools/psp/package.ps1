# Prepares the memory stick folder of the PSP race in build/psp/package (not in git: it holds the game files
# extracted from your own ROM). Copy build\psp\package\PSP to the root of the memory stick yourself; this script
# never writes to a PSP.
#   powershell -ExecutionPolicy Bypass -File tools\psp\package.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $repo
$g = 'build\psp\package\PSP\GAME\TM2PSP'
New-Item -ItemType Directory -Force $g | Out-Null
Copy-Item build\psp\tm_race\EBOOT.PBP "$g\EBOOT.PBP" -Force
robocopy extract\files "$g\files" /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
"easy/0" | Out-File -Encoding ascii "$g\track.txt"
@"
TM2PSP - course Coast (portage de TrackMania Turbo DS)

Installation : copier le dossier PSP de build\psp\package a la racine de la Memory Stick
(ms0:/PSP/GAME/TM2PSP/EBOOT.PBP). Firmware personnalise requis (ex. 6.61 LME / Infinity).
Piste : track.txt (practice/0-4, easy/0-4, medium/0-4, hard/0-4, extreme/0, bonus/0-4).

Commandes : croix = accelerer, carre = freiner / marche arriere, croix directionnelle ou stick = diriger,
triangle = dernier checkpoint, START = recommencer, SELECT = autre camera, R = temps par image.
HOME pour quitter.
"@ | Out-File -Encoding ascii "$g\LISEZMOI.txt"
$size = (Get-ChildItem -Recurse $g | Measure-Object -Property Length -Sum).Sum
Write-Output ("package: {0} ({1:N1} MB)" -f $g, ($size / 1MB))

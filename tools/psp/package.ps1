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
# Only the VFS is read by the race (vfs.zip: models, tracks, scripts; vfs.bin/vfs.dat: its index); audio, movies,
# Download Play and WFC files are not used yet.
if (Test-Path "$g\files") { Remove-Item -Recurse -Force "$g\files" }
New-Item -ItemType Directory -Force "$g\files" | Out-Null
Copy-Item extract\files\vfs.zip, extract\files\vfs.bin, extract\files\vfs.dat "$g\files\"
# No track.txt: the program starts on its track menu (an existing track.txt only places the cursor). save.txt
# (best times) is written by the program on the memory stick and is not part of the package, so copying a new
# package keeps it.
if (Test-Path "$g/track.txt") { Remove-Item "$g/track.txt" }
@"
TM2PSP - course Coast (portage de TrackMania Turbo DS)

Installation : copier le dossier PSP de build\psp\package a la racine de la Memory Stick
(ms0:/PSP/GAME/TM2PSP/EBOOT.PBP). Firmware personnalise requis (ex. 6.61 LME / Infinity).

Menu des pistes (26 pistes Coast, meilleur temps de chacune) : croix directionnelle = choisir
(gauche/droite = 5 pistes d'un coup), croix = jouer, rond = quitter.
Les meilleurs temps sont gardes dans save.txt, a cote de l'EBOOT.

Course : croix = accelerer, carre = freiner / marche arriere, croix directionnelle ou stick = diriger,
triangle = dernier checkpoint, START = pause (continuer, recommencer, piste suivante, menu des pistes),
SELECT = autre camera, R = temps par image (mise a jour / construction / attente GE, en ms).
Apres l'arrivee : recommencer, piste suivante ou menu des pistes. HOME pour quitter.
"@ | Out-File -Encoding ascii "$g\LISEZMOI.txt"
$size = (Get-ChildItem -Recurse $g | Measure-Object -Property Length -Sum).Sum
Write-Output ("package: {0} ({1:N1} MB)" -f $g, ($size / 1MB))

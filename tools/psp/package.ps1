# Prepares the memory stick folder of the PSP race in build/psp/package (not in git: it holds the game files
# extracted from your own ROM). Copy build\psp\package\PSP to the root of the memory stick yourself; this script
# never writes to a PSP.
#   powershell -ExecutionPolicy Bypass -File tools\psp\package.ps1 [-Perf] [-Diag]
# -Perf adds an empty perf.txt: the race then appends its per-second frame times to frametimes.txt.
# -Diag adds diag.txt: at the first launch the race saves diag_<view>_<variant>.bmp (fixed views under bridges, one
# image per rendering variant: see RunDiag in src/psp/race_psp.cpp), deletes diag.txt and goes on to the menu.
param([switch]$Perf, [switch]$Diag)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $repo
$g = 'build\psp\package\PSP\GAME\TM2PSP'
New-Item -ItemType Directory -Force $g | Out-Null
Copy-Item build\psp\tm_race\EBOOT.PBP "$g\EBOOT.PBP" -Force
# The race reads the VFS (vfs.zip: models, tracks, scripts; vfs.bin/vfs.dat: its index) and the sound archive
# (audio/sound_data.sdat); movies, Download Play, WFC and the network sound archive are not used.
if (Test-Path "$g\files") { Remove-Item -Recurse -Force "$g\files" }
New-Item -ItemType Directory -Force "$g\files" | Out-Null
Copy-Item extract\files\vfs.zip, extract\files\vfs.bin, extract\files\vfs.dat "$g\files\"
New-Item -ItemType Directory -Force "$g\files\audio" | Out-Null
Copy-Item extract\files\audio\sound_data.sdat "$g\files\audio\"
# The front end's pictures (tools/fe/convert.py), read from fe/ next to the EBOOT.
if (-not (Test-Path build\fe\data\background.fet)) { python -P tools\fe\convert.py | Out-Null }
if (Test-Path "$g\fe") { Remove-Item -Recurse -Force "$g\fe" }
Copy-Item -Recurse build\fe\data "$g\fe"
# No track.txt: the program starts on its track menu (an existing track.txt only places the cursor). save.txt
# (best times) is written by the program on the memory stick and is not part of the package, so copying a new
# package keeps it.
if (Test-Path "$g/track.txt") { Remove-Item "$g/track.txt" }
if ($Perf) { New-Item -ItemType File -Force "$g\perf.txt" | Out-Null }
elseif (Test-Path "$g\perf.txt") { Remove-Item "$g\perf.txt" }
Remove-Item "$g\diag_*.bmp" -ErrorAction SilentlyContinue
if ($Diag) {
    "sno/easy/0 40 20 300 -16 33 345`nsno/easy/0 110 10 330 -16 33 336`ncoa/easy/0 60 6 80 85 17 115`ncoa/easy/0 200 5 140 240 20 165" |
        Out-File -Encoding ascii "$g\diag.txt"
} elseif (Test-Path "$g\diag.txt") { Remove-Item "$g\diag.txt" }
@"
TM2PSP - mode course (portage de TrackMania Turbo DS)

Installation : copier le dossier PSP de build\psp\package a la racine de la Memory Stick
(ms0:/PSP/GAME/TM2PSP/EBOOT.PBP). Firmware personnalise requis (ex. 6.61 LME / Infinity).

Menu des pistes (les 104 pistes des quatre decors Stadium, Island, Snow et Coast, meilleur temps de
chacune) : haut/bas = choisir la piste, gauche/droite = changer de decor, croix = jouer, rond = quitter.
Les meilleurs temps sont gardes dans save.txt, a cote de l'EBOOT.

Course : croix = accelerer, carre = freiner / marche arriere, croix directionnelle ou stick = diriger,
triangle = dernier checkpoint, START = pause (continuer, recommencer, piste suivante, menu des pistes),
SELECT = autre camera, L = klaxon, R = temps par image (mise a jour / son / construction / attente GE, en ms).
Son : musique, moteur, derapages, chocs, compte a rebours et checkpoints du jeu ; un fichier vide mute.txt
a cote de l'EBOOT coupe le son.
Mesures : un fichier vide perf.txt a cote de l'EBOOT fait ecrire les temps par seconde de course dans
frametimes.txt (ecrit a la pause, au menu et en quittant).
Apres l'arrivee : recommencer, piste suivante ou menu des pistes. HOME pour quitter.
"@ | Out-File -Encoding ascii "$g\LISEZMOI.txt"
$size = (Get-ChildItem -Recurse $g | Measure-Object -Property Length -Sum).Sum
Write-Output ("package: {0} ({1:N1} MB)" -f $g, ($size / 1MB))

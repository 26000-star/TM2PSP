# Imports extract/arm9/arm9.bin into the Ghidra project ghidra/tmturbo, applies the dsd memory map
# and symbols (DsdImport.java), runs full auto-analysis with the NitroSDK FID database attached,
# then writes build/ghidra_stats.txt (DsdStats.java).
# Usage: powershell -File tools\ghidra\analyze.ps1
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$ghidra = if ($env:GHIDRA_INSTALL_DIR) { $env:GHIDRA_INSTALL_DIR } else { 'C:\Tools\ghidra_12.1.4_PUBLIC' }
$fidb = Join-Path $ghidra 'Ghidra\Extensions\NTRGhidra\fidb\nitrosdk3.0.fidb'

New-Item -ItemType Directory -Force "$root\ghidra", "$root\build" | Out-Null

& "$ghidra\support\analyzeHeadless.bat" "$root\ghidra" tmturbo `
    -import "$root\extract\arm9\arm9.bin" -overwrite `
    -loader BinaryLoader -loader-baseAddr 0x02000000 `
    -processor ARM:LE:32:v5t -cspec default `
    -scriptPath "$root\tools\ghidra" `
    -preScript DsdImport.java "$root" "$fidb" `
    -postScript DsdStats.java "$root\build\ghidra_stats.txt" `
    -log "$root\build\ghidra.log" -scriptlog "$root\build\ghidra_script.log"
exit $LASTEXITCODE

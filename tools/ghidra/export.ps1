# Runs ExportAnalysis.java on the analyzed program (no re-import, no re-analysis) and writes
# build/analysis/functions.jsonl. Usage: powershell -File tools\ghidra\export.ps1
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$ghidra = if ($env:GHIDRA_INSTALL_DIR) { $env:GHIDRA_INSTALL_DIR } else { 'C:\Tools\ghidra_12.1.4_PUBLIC' }
New-Item -ItemType Directory -Force "$root\build\analysis" | Out-Null

& "$ghidra\support\analyzeHeadless.bat" "$root\ghidra" tmturbo -process arm9.bin -noanalysis -readOnly `
    -scriptPath "$root\tools\ghidra" `
    -postScript ExportAnalysis.java "$root\build\analysis\functions.jsonl" `
    -log "$root\build\ghidra_export.log"
exit $LASTEXITCODE

# Decompiles functions from the analyzed Ghidra project to a C file.
# Usage: powershell -File tools\ghidra\decompile.ps1 <out.c> <selector> [<selector> ...]
#   selectors: name | 0x02012345 | ov003:0x020f8000 | str:<text> | callers:<selector>
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$ghidra = if ($env:GHIDRA_INSTALL_DIR) { $env:GHIDRA_INSTALL_DIR } else { 'C:\Tools\ghidra_12.1.4_PUBLIC' }
$out = [IO.Path]::GetFullPath($args[0])
$sel = $args[1..($args.Length - 1)]
& "$ghidra\support\analyzeHeadless.bat" "$root\ghidra" tmturbo -process arm9.bin -noanalysis -readOnly `
    -scriptPath "$root\tools\ghidra" -postScript Decompile.java $out @sel `
    -log "$root\build\ghidra_decompile.log" | Select-String 'Decompile:|ERROR' | ForEach-Object { $_.Line }
exit $LASTEXITCODE

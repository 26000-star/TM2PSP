#!/bin/bash
# Names NitroSDK / NitroSystem functions in the ARM9 binary by byte signatures.
# 1. ntrtwl/NitroSDK 4.2.30001 and ntrtwl/NitroSystem 071126 at the revisions pret uses;
# 2. mid-kid/metroskrew 0.1.3 (the original CodeWarrior ARM compilers, as used by the pret decomps;
#    local analysis tool only, never shipped with the port);
# 3. builds both with "-wrap:sdk 2.0/sp2p2" and matches every function's masked bytes.
# Needs: git, curl, 7-Zip (Windows), python with meson==1.12.0 and pyelftools, ninja, gcc.
# Run from the repository root (Git Bash): bash tools/analysis/sdk_names.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
EXT=build/ext
mkdir -p "$EXT" build/analysis
MESON="python -m mesonbuild.mesonmain"

clone() {  # url dir revision
    if [ ! -d "$EXT/$2" ]; then
        git clone -q --filter=blob:none "$1" "$EXT/$2"
        git -C "$EXT/$2" checkout -q "$3"
    fi
}
clone https://github.com/ntrtwl/NitroSDK.git NitroSDK eaae40f199c0a8827947afbc33227b6ba2f9d45c
clone https://github.com/ntrtwl/NitroSystem.git NitroSystem 21c8fd078fdda34f3c2622ef0751d00adaacf890

# Host tools (makelcf needs flex) and ARM7 components are not needed for ARM9 signatures.
sed -i "s/^subdir('tools')$/# subdir('tools')  # TM2PSP: host tools not needed/; s/^subdir('components')$/# subdir('components')  # TM2PSP: needs makelcf/" "$EXT/NitroSDK/meson.build"

if [ ! -x "$EXT/metroskrew/bin/mwccarm.exe" ]; then
    mkdir -p "$EXT/dl-metroskrew"
    curl -sSL --fail -o "$EXT/dl-metroskrew/metroskrew-windows.7z" \
        https://github.com/mid-kid/metroskrew/releases/download/0.1.3/metroskrew-windows.7z
    echo "e357afb842df4d8265828fb88a6722a0538df8d79bcb0cf01247c0ea6bb450f6  $EXT/dl-metroskrew/metroskrew-windows.7z" | sha256sum -c -
    "/c/Program Files/7-Zip/7z.exe" x -y -o"$EXT/dl-metroskrew/x" "$EXT/dl-metroskrew/metroskrew-windows.7z" > /dev/null
    mv "$EXT/dl-metroskrew/x/metroskrew" "$EXT/metroskrew"
fi

T="$(cd "$EXT/metroskrew/bin" && pwd -W)"
FLAGS="'-O4,p', '-proc', 'arm946e', '-enum', 'int', '-lang', 'c99', '-Cpp_exceptions', 'off', '-gccext,on', '-msgstyle', 'gcc', '-ipa', 'file', '-interworking', '-inline', 'on,noauto', '-char', 'signed', '-nosyspath', '-stdinc'"
cat > "$EXT/cross-mwcc.ini" <<EOF
[binaries]
c      = ['$T/mwccarm.exe',  '-wrap:sdk', '2.0/sp2p2']
c_ld   = ['$T/mwldarm.exe',  '-wrap:sdk', '2.0/sp2p2']
nasm   = ['$T/mwasmarm.exe', '-wrap:sdk', '2.0/sp2p2']
ar     = ['$T/mwldarm.exe',  '-wrap:sdk', '2.0/sp2p2']
strip  = ['false']

[host_machine]
system     = 'bare metal'
cpu_family = 'arm'
cpu        = 'arm'
endian     = 'little'
EOF
# NitroSystem pulls NitroSDK as a subproject, which then gets no flags of its own: pass them once
# through the cross file and use buildtype=plain so neither project adds them again.
{ cat "$EXT/cross-mwcc.ini"; printf "\n[built-in options]\nc_args = [%s]\n" "$FLAGS"; } > "$EXT/cross-mwcc-nnsys.ini"
printf "[binaries]\nc = 'gcc'\nar = 'ar'\n" > "$EXT/native.ini"

if [ ! -d "$EXT/build-nitrosdk" ]; then
    (cd "$EXT/NitroSDK" && $MESON setup ../build-nitrosdk --cross-file ../cross-mwcc.ini --native-file ../native.ini -Dbuildtype=release)
fi
ninja -C "$EXT/build-nitrosdk" > /dev/null

if [ ! -e "$EXT/NitroSystem/subprojects/NitroSDK" ]; then
    powershell -NoProfile -Command "New-Item -ItemType Junction -Path '$(cd "$EXT" && pwd -W)/NitroSystem/subprojects/NitroSDK' -Target '$(cd "$EXT" && pwd -W)/NitroSDK' | Out-Null"
fi
if [ ! -d "$EXT/build-nitrosystem" ]; then
    (cd "$EXT/NitroSystem" && $MESON setup ../build-nitrosystem --cross-file ../cross-mwcc-nnsys.ini --native-file ../native.ini -Dbuildtype=plain --wrap-mode=nodownload)
fi
ninja -C "$EXT/build-nitrosystem" > /dev/null

python -P tools/analysis/sig_match.py build/analysis/sdk_sigs.txt "$EXT/build-nitrosdk/libraries" "$EXT/build-nitrosystem/libraries"
python -P tools/analysis/apply_names.py build/analysis/sdk_sigs.txt

#!/bin/bash
# Builds a PSP CMake project with pspdev inside the WSL distro Ubuntu-24.04.
# Usage (from Git Bash, repository root):
#   bash tools/psp_build.sh                the game (root CMakeLists.txt): EBOOTs in build/psp/<program>/EBOOT.PBP
#   bash tools/psp_build.sh psp/viewer     the phase 2 model viewer: build/psp/viewer/EBOOT.PBP
# The build tree lives in the distro's file system (~/tm2psp-build/<name>, much faster than /mnt/c); only the
# EBOOTs and the ELFs (for addr2line / size) are copied back.
# Environment: TM_PSP_BUILD_TYPE (default Release), TM_PSP_VERBOSE=1 to show the full compiler output.
set -euo pipefail
cd "$(dirname "$0")/.."
SRC="${1:-.}"
if [ "$SRC" = "." ]; then NAME=game; else NAME="$(basename "$SRC")"; fi
TYPE="${TM_PSP_BUILD_TYPE:-Release}"
WSL_ROOT="$(MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -u root -- wslpath -a "$(pwd -W)" | tr -d '\r')"
mkdir -p build/psp
FILTER="grep -E 'error|warning|Built target|EBOOT|undefined' || true"
if [ "${TM_PSP_VERBOSE:-0}" = 1 ]; then FILTER=cat; fi
# wsl.exe mangles multi-line -c arguments, so the inner commands go through a script file.
cat > build/psp/.build_inner.sh <<EOF
set -eo pipefail
B="\$HOME/tm2psp-build/$NAME"
mkdir -p "\$B"
cd "\$B"
psp-cmake -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=$TYPE '$WSL_ROOT/$SRC' > /dev/null
make -j\$(nproc) 2>&1 | $FILTER
test \${PIPESTATUS[0]} -eq 0
for pbp in \$(find . -name EBOOT.PBP -newer CMakeCache.txt -o -name EBOOT.PBP -path '*_pbp/*'); do
  d="\$(basename "\$(dirname "\$pbp")")"
  d="\${d%_pbp}"
  [ "\$d" = "." ] && d="$NAME"
  mkdir -p '$WSL_ROOT/build/psp/'"\$d"
  cp "\$pbp" '$WSL_ROOT/build/psp/'"\$d/"
  [ -f "\$d" ] && cp "\$d" '$WSL_ROOT/build/psp/'"\$d/\$d.elf" || true
done
EOF
MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -u root -- bash -l "$WSL_ROOT/build/psp/.build_inner.sh"
ls -la build/psp/*/EBOOT.PBP

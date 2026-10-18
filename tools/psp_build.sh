#!/bin/bash
# Builds a PSP CMake project with pspdev inside the WSL distro Ubuntu-24.04.
# Usage (from Git Bash, repository root): bash tools/psp_build.sh psp/viewer
# Output: build/psp/<project name>/EBOOT.PBP
set -euo pipefail
cd "$(dirname "$0")/.."
SRC="$1"
NAME="$(basename "$SRC")"
WSL_ROOT="$(MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -u root -- wslpath -a "$(pwd -W)" | tr -d '\r')"
mkdir -p build/psp
# wsl.exe mangles multi-line -c arguments, so the inner commands go through a script file.
cat > build/psp/.build_inner.sh <<EOF
set -e
B="\$HOME/tm2psp-build/$NAME"
mkdir -p "\$B"
cd "\$B"
psp-cmake -G 'Unix Makefiles' '$WSL_ROOT/$SRC' > /dev/null
make 2>&1 | grep -E 'error|warning|Built target|EBOOT' || true
mkdir -p '$WSL_ROOT/build/psp/$NAME'
cp EBOOT.PBP '$WSL_ROOT/build/psp/$NAME/'
EOF
MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -u root -- bash -l "$WSL_ROOT/build/psp/.build_inner.sh"
ls -la "build/psp/$NAME/EBOOT.PBP"

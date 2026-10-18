#!/bin/bash
# Recovers Lua 5.0.2 function names in the ARM9 main module and writes them to config/arm9/symbols.txt.
# Needs: build/analysis/functions.jsonl (tools/ghidra/export.ps1), gcc + objdump on PATH, curl, python.
# Run from the repository root: bash tools/analysis/lua_names.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
EXT=build/ext
LUA=$EXT/lua-5.0.2
mkdir -p "$EXT" build/analysis

if [ ! -d "$LUA/src" ]; then
    curl -sSL --fail -o "$EXT/lua-5.0.2.tar.gz" https://www.lua.org/ftp/lua-5.0.2.tar.gz
    echo "dea74646b7e5c621fef7174df83c34b1  $EXT/lua-5.0.2.tar.gz" | md5sum -c -
    # test/ contains symlinks that fail on Windows; only src/ and include/ are needed.
    tar xzf "$EXT/lua-5.0.2.tar.gz" -C "$EXT" lua-5.0.2/src lua-5.0.2/include
fi

# Host objects: -O0 and no inlining keep every call as a relocation; one section per function.
mkdir -p "$EXT/lua-obj"
for f in "$LUA"/src/*.c "$LUA"/src/lib/*.c; do
    gcc -O0 -fno-inline -ffunction-sections -fdata-sections -DLUA_NUMBER=int \
        -I"$LUA/include" -I"$LUA/src" -c "$f" -o "$EXT/lua-obj/$(basename "$f" .c).o"
done

python -P tools/analysis/find_reg_tables.py > /dev/null

# Link order observed in the ROM: alphabetical, without the parser (lcode/llex/lparser are not linked)
# and with lmathlib replaced by Firebrand's fixed-point math (named from its luaL_reg table below).
python -P tools/analysis/align_lib.py "$LUA/src" "$EXT/lua-obj" arm9_main 0203ad78 02041e90 \
    build/analysis/lua_align.txt --tables build/analysis/reg_tables.txt --reverse \
    --order lapi.c,lauxlib.c,lbaselib.c,ldebug.c,ldo.c,lfunc.c,lgc.c,lmem.c,lobject.c,lstate.c,lstring.c,ltable.c,ltablib.c,ltm.c,lundump.c,lvm.c,lzio.c

python -P - <<'EOF'
import re
out = open("build/analysis/lua_custom_math.txt", "w")
take = False
for l in open("build/analysis/reg_tables.txt"):
    if l.startswith("=="):
        take = " 020b8ebc " in l
        continue
    m = re.match(r"\s+main:([0-9a-f]{8}) (\S+)$", l)
    if take and m:
        out.write("%s fxmath_%s # luaL_reg table at 020b8ebc\n" % (m.group(1), m.group(2)))
out.write("02041e30 luaopen_fxmath # registers the table above; called once from game init\n")
EOF

python -P tools/analysis/apply_names.py build/analysis/lua_align.txt build/analysis/lua_custom_math.txt

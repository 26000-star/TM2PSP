"""Checks tm_cfgdump --carparams against the unluac decompilation of Scripts/config/vehicles.luac.

The decompiled script (assets/lua_src/Scripts/config/vehicles.lua, made with unluac in phase 1) is pure data:
assignments of numbers, strings and tables to locals, fields and globals. This script evaluates it independently
of the port and compares every loaded field, the wheel vectors and SaveTime.

Usage: python -P tools/host/check_carparams.py <cfgdump output> [assets/lua_src/Scripts/config/vehicles.lua]
"""
import re
import sys

# Fields the game stores as int (fx32 >> 12); all others are stored as loaded.
INT_FIELDS = {"RPMMin", "RPMMax", "NumGears"}
# Keys present in the script that the DS game never registers (probably shared with the Wii version).
IGNORED = {"WiimoteMode"}


def eval_lua_data(path):
    env = {}  # locals and globals
    def value(tok):
        tok = tok.strip()
        if tok == "{}":
            return {}
        if re.fullmatch(r"-?\d+", tok):
            return int(tok)
        if tok.startswith('"'):
            return tok[1:-1]
        return env[tok]
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("local "):
            continue
        lhs, rhs = [s.strip() for s in line.split("=", 1)]
        v = value(rhs)
        m = re.fullmatch(r"(\w+)\.(\w+)", lhs) or re.fullmatch(r"(\w+)\[(\d+)\]", lhs)
        if m:
            key = int(m.group(2)) if lhs.endswith("]") else m.group(2)
            env[m.group(1)][key] = v
        else:
            env[lhs] = v
    return env


def parse_dump(path):
    cars, save = {}, None
    cur = None
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] == "SaveTime":
            save = p[1]
        elif p[0] == "car":
            cur = cars.setdefault(p[1], {})
        elif p[0] in ("FrontWheel", "RearWheel"):
            cur[p[0]] = [int(x) for x in p[1:4]]
        elif p[0] != "derived":
            cur[p[0]] = int(p[1])
    return cars, save


def main():
    dump = sys.argv[1]
    src = sys.argv[2] if len(sys.argv) > 2 else "assets/lua_src/Scripts/config/vehicles.lua"
    env = eval_lua_data(src)
    expected = env["carParams"]
    got, save = parse_dump(dump)
    errors = 0
    if save != env.get("SaveTime"):
        print("SaveTime: got %r, expected %r" % (save, env.get("SaveTime")))
        errors += 1
    if sorted(got) != sorted(expected):
        print("cars: got %s, expected %s" % (sorted(got), sorted(expected)))
        errors += 1
    checked = 0
    for car, fields in expected.items():
        g = got.get(car, {})
        for k, v in fields.items():
            if k in IGNORED:
                continue
            if isinstance(v, dict):
                v = [v[1], v[2], v[3]]
            elif k in INT_FIELDS:
                v >>= 12
            checked += 1
            if g.get(k) != v:
                print("%s.%s: got %r, expected %r" % (car, k, g.get(k), v))
                errors += 1
        missing = set(g) - set(fields)
        for k in sorted(missing):
            print("%s.%s: loaded but absent from the script (value %r)" % (car, k, g[k]))
    print("%d cars, %d values checked, %d errors" % (len(expected), checked, errors))
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()

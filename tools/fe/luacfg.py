"""Reads the game's config scripts as decompiled by unluac (assets/lua_src/Scripts/config/*.lua): only constant
assignments to locals, table fields and globals. Numbers stay as the game stores them (fx32: 4096 = 1.0).

Usage: python -P tools/fe/luacfg.py <name> [--fx]     prints the globals of Scripts/config/<name>.lua as JSON
       (--fx divides integer numbers by 4096)
"""
import json
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
CFG = os.path.join(ROOT, "assets", "lua_src", "Scripts", "config")

_ASSIGN = re.compile(r'^\s*([A-Za-z_][A-Za-z0-9_]*)((?:\[[^\]]+\]|\.[A-Za-z_][A-Za-z0-9_]*)*)\s*=\s*(.+?)\s*$')
_PART = re.compile(r'\[([^\]]+)\]|\.([A-Za-z_][A-Za-z0-9_]*)')


def _value(expr, env):
    expr = expr.strip()
    if expr == "{}":
        return {}
    if expr in ("true", "false"):
        return expr == "true"
    if expr == "nil":
        return None
    if expr.startswith('"') and expr.endswith('"'):
        return bytes(expr[1:-1], "latin-1").decode("unicode_escape")
    try:
        return int(expr)
    except ValueError:
        pass
    try:
        return float(expr)
    except ValueError:
        pass
    if expr.startswith("-") and expr[1:] in env:
        return -env[expr[1:]]
    return env[expr] if expr in env else _GLOBALS[expr]


_GLOBALS = {}


def load(name):
    env, glob = {}, _GLOBALS
    with open(os.path.join(CFG, name + ".lua"), encoding="latin-1") as f:
        for line in f:
            if line.startswith("local "):
                continue
            m = _ASSIGN.match(line)
            if not m:
                continue
            base, path, expr = m.groups()
            v = _value(expr, env)
            if not path:
                if re.match(r'^L\d+_1$', base):
                    env[base] = v
                else:
                    glob[base] = v
                continue
            t = env[base] if base in env else glob[base]
            parts = _PART.findall(path)
            for i, (idx, field) in enumerate(parts):
                key = _value(idx, env) if idx else field
                if i == len(parts) - 1:
                    t[key] = v
                else:
                    t = t[key]
    return glob


def _list(t):
    """A table with keys 1..n as a list (recursively)."""
    if isinstance(t, dict):
        t = {k: _list(v) for k, v in t.items()}
        if t and all(isinstance(k, int) for k in t) and sorted(t) == list(range(1, len(t) + 1)):
            return [t[k] for k in range(1, len(t) + 1)]
    return t


def _fx(v):
    if isinstance(v, dict):
        return {k: _fx(x) for k, x in v.items()}
    if isinstance(v, list):
        return [_fx(x) for x in v]
    if isinstance(v, int) and not isinstance(v, bool):
        return v / 4096
    return v


if __name__ == "__main__":
    g = _list(load(sys.argv[1]))
    if "--fx" in sys.argv:
        g = _fx(g)
    print(json.dumps(g, indent=1, ensure_ascii=False))

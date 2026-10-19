"""Checks tm_physics --raycast against a float ray cast of the same triangles.

Each placed piece's collision mesh is read from assets/vfs (independent parser), its vertices are moved to world
space with the dumped piece matrix in double precision, and every probe is intersected with every triangle
(Moller-Trumbore, front faces only, 0 <= t <= length). Per probe and per piece, the port's hit must match the
nearest float hit: same piece set, same material, normal within 8/4096, and t within 1/64 unit of the distance to
the plane and ray the game uses: the probe moved to the mesh's local space with the integer MTX_Inverse43 and the
truncating vector * matrix products, against the rounded normal (e1 x e2) >> 12. (Grazing rays are sensitive to the
1/4096 truncation of the local direction: up to 0.05 unit at 20 units on walls.) The exact world-space plane must
agree within max(0.1, 0.5%).
Probes whose float hit lies within 0.01 of a triangle edge (or nearly parallel) may differ and are counted apart.

Usage: python -P tools/host/check_raycast.py <tm_physics --raycast output>
"""
import os
import struct
import sys

VFS = "assets/vfs"
EPS_EDGE = 0.01


def find_h(model):
    want = (model + ".h").lower().replace("/", os.sep)
    for root, _, files in os.walk(os.path.join(VFS, "Graphics")):
        for f in files:
            p = os.path.join(root, f)
            if p.lower().endswith(want):
                return p
    return None


def load_h(path):
    d = open(path, "rb").read()
    ver, nv, nt, fl = struct.unpack_from("<HHHh", d, 0)
    verts = [struct.unpack_from("<3i", d, 8 + 12 * i) for i in range(nv)]
    o = 8 + 12 * nv
    tris = [struct.unpack_from("<3H", d, o + 6 * i) for i in range(nt)]
    o += 6 * nt
    nc, = struct.unpack_from("<I", d, o)
    o += 4
    if nc:
        o += 2 * nc + 36
        nn, = struct.unpack_from("<I", d, o)
        o += 4 + 4 * nn
    mats = d[o:o + nt] if fl else None
    return verts, tris, mats


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def rn12(v):
    return (v + 0x800) >> 12


def inverse43(a):
    """MTX_Inverse43 (SDK 0x0202b294) on integers: first-row cofactors rounded, the rest truncated."""
    sh = lambda v: v >> 12
    c1 = rn12(a[3] * a[8] - a[5] * a[6])
    c0 = rn12(a[4] * a[8] - a[5] * a[7])
    c2 = rn12(a[3] * a[7] - a[4] * a[6])
    det = rn12(a[0] * c0 - a[1] * c1 + c2 * a[2])
    q = abs(4096 << 32) // abs(det) * (1 if det > 0 else -1)
    inv = (q + (1 << 19)) >> 20
    o = [0] * 12
    o[0] = sh(c0 * inv)
    o[1] = -sh(sh(a[1] * a[8] - a[7] * a[2]) * inv)
    o[2] = sh(sh(a[1] * a[5] - a[4] * a[2]) * inv)
    o[3] = -sh(c1 * inv)
    o[4] = sh(sh(a[0] * a[8] - a[6] * a[2]) * inv)
    o[5] = -sh(sh(a[0] * a[5] - a[3] * a[2]) * inv)
    o[6] = sh(c2 * inv)
    o[7] = -sh(sh(a[0] * a[7] - a[6] * a[1]) * inv)
    o[8] = sh(sh(a[0] * a[4] - a[3] * a[1]) * inv)
    o[9] = -sh(a[11] * o[6] + a[9] * o[0] + o[3] * a[10])
    o[10] = -sh(a[11] * o[7] + a[9] * o[1] + o[4] * a[10])
    o[11] = -sh(a[11] * o[8] + a[9] * o[2] + o[5] * a[10])
    return o


def mul43(v, m, translate):
    r = [(v[0] * m[0] + v[1] * m[3] + v[2] * m[6]) >> 12, (v[0] * m[1] + v[1] * m[4] + v[2] * m[7]) >> 12,
         (v[0] * m[2] + v[1] * m[5] + v[2] * m[8]) >> 12]
    if translate:
        r = [r[0] + m[9], r[1] + m[10], r[2] + m[11]]
    return r


def main():
    pieces, rays = [], []
    for line in open(sys.argv[1]):
        w = line.split()
        if not w:
            continue
        if w[0] == "piece":
            mi = [int(v) for r in w[3][4:].split(";") for v in r.split(",")]
            m = [[v / 4096.0 for v in mi[i * 3:i * 3 + 3]] for i in range(4)]
            pieces.append((w[2], m, inverse43(mi)))
        elif w[0] == "ray":
            oi = [int(v) for v in w[1].split(",")]
            di = [int(v) for v in w[2].split(",")]
            o = tuple(v / 4096.0 for v in oi)
            d = tuple(v / 4096.0 for v in di)
            ln = int(w[3]) / 4096.0
            hits = []
            rest = line[line.index("hits="):].split("[")[1:]
            for h in rest:
                f = h.strip().rstrip("]").split()
                hits.append((int(f[0]) / 4096.0, tuple(int(v) / 4096.0 for v in f[1].split(",")),
                             tuple(int(v) / 4096.0 for v in f[2].split(",")), int(f[3]), int(f[4])))
            rays.append((o, d, ln, hits, oi, di))
    world = []
    cache = {}
    for model, m, minv in pieces:
        if model not in cache:
            p = find_h(model)
            cache[model] = load_h(p) if p else None
        mesh = cache[model]
        if mesh is None:
            world.append(None)
            continue
        verts, tris, mats = mesh
        wv, ln_ = [], []
        for v in verts:
            x, y, z = (c / 4096.0 for c in v)
            wv.append((x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0],
                       x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1],
                       x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2]))
        lo = [min(v[i] for v in wv) for i in range(3)]
        hi = [max(v[i] for v in wv) for i in range(3)]
        # the game's rounded face normals, rotated to world space
        rn = []
        for a, b, c in tris:
            A, B, C = verts[a], verts[b], verts[c]
            e1, e2 = sub(B, A), sub(C, A)
            n = cross(e1, e2)
            n = tuple(((v + 0x800) >> 12) for v in n)
            rn.append((n[0] * m[0][0] + n[1] * m[1][0] + n[2] * m[2][0],
                       n[0] * m[0][1] + n[1] * m[1][1] + n[2] * m[2][1],
                       n[0] * m[0][2] + n[1] * m[1][2] + n[2] * m[2][2]))
        # local-space data for the game's own plane: integer vertices and rounded normals
        lt = []
        for a, b, c in tris:
            A, B, C = verts[a], verts[b], verts[c]
            n = tuple(((v + 0x800) >> 12) for v in cross(sub(B, A), sub(C, A)))
            lt.append((A, n))
        world.append((wv, tris, mats, lo, hi, rn, minv, lt))

    errors = ambiguous = checked = hits_total = 0
    for o, d, ln, got, oi, di in rays:
        expect = {}
        edge = False
        end = (o[0] + d[0] * ln, o[1] + d[1] * ln, o[2] + d[2] * ln)
        for si, sh in enumerate(world):
            if sh is None:
                continue
            wv, tris, mats, lo, hi, rn, minv, lt = sh
            lo_ = mul43(oi, minv, True)
            ld_ = mul43(di, minv, False)
            if any(max(o[i], end[i]) < lo[i] - 0.1 or min(o[i], end[i]) > hi[i] + 0.1 for i in range(3)):
                continue
            best = None
            for ti, (a, b, c) in enumerate(tris):
                A, B, C = wv[a], wv[b], wv[c]
                e1, e2 = sub(B, A), sub(C, A)
                n = cross(e1, e2)
                det = dot(n, d)
                if det >= 0:
                    continue
                s = sub(o, A)
                t = dot(n, s) / -det
                p = (o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t)
                # barycentric coordinates of p
                v0, v1, v2 = e1, e2, sub(p, A)
                d00, d01, d11, d20, d21 = dot(v0, v0), dot(v0, v1), dot(v1, v1), dot(v2, v0), dot(v2, v1)
                den = d00 * d11 - d01 * d01
                if den == 0:
                    continue
                bv = (d11 * d20 - d01 * d21) / den
                bw = (d00 * d21 - d01 * d20) / den
                bu = 1 - bv - bw
                inside = min(bu, bv, bw) >= 0
                near = min(abs(bu), abs(bv), abs(bw)) < EPS_EDGE or abs(det) < 1e-3 or \
                    abs(t) < 0.01 or abs(t - ln) < 0.01
                if not (0 <= t <= ln) and not (near and -0.01 <= t <= ln + 0.01):
                    continue
                if inside or near:
                    if near:
                        edge = True
                    if inside and 0 <= t <= ln and (best is None or t < best[0]):
                        nl = (n[0] ** 2 + n[1] ** 2 + n[2] ** 2) ** 0.5
                        # the game's plane and ray, in the mesh's local integer space
                        LA, g = lt[ti]
                        gd = dot(g, ld_)
                        tg = dot(g, sub(lo_, LA)) / -gd if gd else t
                        best = (t, p, (n[0] / nl, n[1] / nl, n[2] / nl), mats[ti] if mats else 255, tg)
            if best:
                expect[si] = best
        gotmap = {h[4]: h for h in got}
        checked += 1
        hits_total += len(got)
        bad = []
        if set(gotmap) != set(expect):
            bad.append("pieces %s vs float %s" % (sorted(gotmap), sorted(expect)))
        for si in set(gotmap) & set(expect):
            t, p, n, mat, _ = gotmap[si]
            et, ep, en, emat, etg = expect[si]
            if abs(t - etg) > 1 / 64 or abs(t - et) > max(0.1, et * 0.005):
                bad.append("piece %d t %.4f vs %.4f (game plane %.4f)" % (si, t, et, etg))
            elif max(abs(p[i] - (o[i] + d[i] * t)) for i in range(3)) > 1 / 64:
                bad.append("piece %d point %s off the ray" % (si, p))
            elif max(abs(n[i] - en[i]) for i in range(3)) > 8 / 4096:
                bad.append("piece %d normal %s vs %s" % (si, n, en))
            elif mat != emat:
                bad.append("piece %d material %d vs %d" % (si, mat, emat))
        if bad:
            if edge:
                ambiguous += 1
            else:
                errors += 1
                if errors <= 20:
                    print("ray o=%s d=%s: %s" % (o, d, "; ".join(bad)))
    print("%d probes, %d hits, %d edge cases, %d errors" % (checked, hits_total, ambiguous, errors))
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()

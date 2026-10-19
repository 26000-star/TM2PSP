"""Checks `tm_physics --camera` (Coast Chase1 camera behind the car of coa/easy/0). Behaviour only: no DS trace.

  - every frame: the camera frame is orthonormal (rows within 0.01 of unit length and of each other's normal) and
    looks at the car (forward within 3 degrees of the direction to the car position + LookAtHeight up; 8 degrees
    until the car has been level for 15 frames, since the camera uses its own smoothed up);
  - standing during the countdown (from frame 30): Distance behind the car and Height above it, within 0.05;
  - full throttle: the distance spring stays in [0, 1.05], the camera stays behind the car and, while the car is
    level, between Distance - 0.05 and Distance + DistInc + 0.1 from it across the up axis;
  - braking then reversing: the camera turns round (reverse blend above 0) and ends in front of the car.
Usage: python -P tools/host/check_camera.py <dump>
"""
import math
import sys


def v3(s):
    return [int(c) / 4096 for c in s.split(",")]


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def sub(a, b):
    return [x - y for x, y in zip(a, b)]


def norm(a):
    return math.sqrt(dot(a, a))


def main():
    frames, prm = [], None
    for line in open(sys.argv[1]):
        w = line.split()
        if w and w[0] == "camera":
            prm = {k: int(v) / 4096 for k, v in (x.split("=") for x in w[1:])}
        elif w and w[0] == "frame":
            kv = dict(x.split("=") for x in w[2:])
            f = {k: v3(kv[k]) for k in ("car", "fwd", "up", "cam", "r", "u", "f")}
            f.update(n=int(w[1]), phase=int(kv["phase"]), speed=int(kv["fwdspeed"]) / 4096,
                     rev=int(kv["rev"]) / 4096, spring=int(kv["spring"]) / 4096)
            frames.append(f)
    errors = []
    flat = 0
    for f in frames:
        r, u, fw = f["r"], f["u"], f["f"]
        if any(abs(norm(x) - 1) > 0.01 for x in (r, u, fw)) or any(abs(dot(a, b)) > 0.01 for a, b in
                                                                   ((r, u), (r, fw), (u, fw))):
            errors.append("frame %d: frame not orthonormal" % f["n"])
        tgt = [c + k * prm["LookAtHeight"] for c, k in zip(f["car"], f["up"])]
        d = sub(tgt, f["cam"])
        ang = math.degrees(math.acos(max(-1, min(1, dot(d, fw) / norm(d)))))
        flat = flat + 1 if f["up"][1] > 0.99 else 0
        level = flat >= 15
        if ang > (3 if level else 8):
            errors.append("frame %d: looks %.1f degrees away from the car" % (f["n"], ang))
        rel = sub(f["cam"], f["car"])
        along, up = dot(rel, f["fwd"]), dot(rel, f["up"])
        across = math.sqrt(max(0, dot(rel, rel) - up * up))
        if f["phase"] == 0 and f["n"] >= 30:
            if abs(along + prm["Distance"]) > 0.05 or abs(up - prm["Height"]) > 0.05:
                errors.append("frame %d standing: %.3f along, %.3f up" % (f["n"], along, up))
        if f["phase"] == 1:
            if not -0.001 <= f["spring"] <= 1.05:
                errors.append("frame %d: spring %.3f" % (f["n"], f["spring"]))
            if along >= 0 or level and not prm["Distance"] - 0.05 <= across <= prm["Distance"] + prm["DistInc"] + 0.1:
                errors.append("frame %d accelerating: %.3f along, %.3f across" % (f["n"], along, across))
    last = frames[-1]
    if last["phase"] != 2 or last["speed"] > -2 or last["rev"] <= 0 or dot(sub(last["cam"], last["car"]),
                                                                           last["fwd"]) <= 0:
        errors.append("reversing: the camera did not come round (%s)" % {k: last[k] for k in ("speed", "rev")})
    for e in errors[:40]:
        print("ERROR", e)
    print("%d frames, %d errors" % (len(frames), len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

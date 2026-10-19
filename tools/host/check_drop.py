"""Checks the output of `tm_physics --drop` (the Coast car dropped on the start platform of coa/easy/0).

Properties checked (there is no recorded trace of the DS to compare with):
  - the car lands on its wheels and settles: during steps 60..89 all four wheels are down, the body stays level
    (up.y >= 0.99), the height varies by less than 0.05 and the horizontal speed stays under 0.25;
  - full throttle from step 90: the forward speed never drops by more than 0.1 per step (slope changes pitch the car) until the first body contact or the
    first gear change, the box goes up at least to 2nd gear, and the speed reaches 15 m/s within 3 s;
  - braking from step 240: the car stops, then the automatic box selects reverse and the car backs up.
Usage: python -P tools/host/check_drop.py <tm_physics --drop output>
"""
import re
import sys


def main():
    steps = []
    for line in open(sys.argv[1]):
        m = re.match(r"step (\d+) pos=(\S+) up=(\S+) v=(\S+) speed=(-?\d+) fwd=(-?\d+) wheels=(\d+) gear=(-?\d+) "
                     r"rpm=(-?\d+) hits=(\d+)", line)
        if not m:
            continue
        vec = lambda s: [int(x) for x in s.split(",")]
        steps.append(dict(n=int(m.group(1)), pos=vec(m.group(2)), up=vec(m.group(3)), v=vec(m.group(4)),
                          fwd=int(m.group(6)), wheels=int(m.group(7)), gear=int(m.group(8)), hits=int(m.group(10))))
    errors = []
    if len(steps) != 300:
        errors.append("expected 300 steps, got %d" % len(steps))
    settle = steps[60:90]
    hs = [s["pos"][1] for s in settle]
    if any(s["wheels"] != 4 for s in settle):
        errors.append("settle: not all wheels down")
    if any(s["up"][1] < 4055 for s in settle):
        errors.append("settle: body not level")
    if max(hs) - min(hs) > 205:
        errors.append("settle: height varies by %d" % (max(hs) - min(hs)))
    if any((s["v"][0] ** 2 + s["v"][2] ** 2) ** 0.5 > 1024 for s in settle):
        errors.append("settle: horizontal speed")
    prev = None
    top_gear = 0
    reached = None
    for s in steps[91:240]:
        if s["hits"]:
            break
        if prev is not None and s["gear"] == prev["gear"] and s["fwd"] < prev["fwd"] - 410:
            errors.append("throttle: speed dropped at step %d" % s["n"])
        top_gear = max(top_gear, s["gear"])
        if reached is None and s["fwd"] >= 15 << 12:
            reached = s["n"]
        prev = s
    if top_gear < 2:
        errors.append("throttle: no gear change")
    if reached is None or reached > 90 + 90:
        errors.append("throttle: 15 m/s not reached within 3 s")
    brake = steps[240:]
    stop = next((s["n"] for s in brake if abs(s["fwd"]) < 0x400), None)
    if stop is None:
        errors.append("brake: the car does not stop")
    if not any(s["gear"] == -1 and s["fwd"] < -(2 << 12) for s in brake):
        errors.append("brake: no reverse")
    print("settle height %d..%d, gear %d, 15 m/s at step %s, stop at step %s" % (min(hs), max(hs), top_gear,
                                                                                  reached, stop))
    for e in errors:
        print("ERROR", e)
    print("%d errors" % len(errors))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

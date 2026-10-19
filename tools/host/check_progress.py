"""Checks `tm_physics --progress` (race progress scenario on one Coast track).

The expected sequence is rebuilt from the race line (checkpoint and lap counts, which check_triggers.py validates
against the catalogue and the scene):
  - restart, then "go" once the 3 s countdown has run out: 3.0 / (2 x 1/60 s) -> frame 90 (91st frame);
  - the car settles on the start point on four wheels, level, at most 3 units above it;
  - crossing the finish first: a finish track reports every checkpoint missed, a lap track nothing (the car has not
    left the start piece);
  - checkpoints in order (a second pass of cp0 counts nothing); a respawn after cp0 puts the car on cp0's start
    point (same x/z, at most 3 units higher: ground probe); lap tracks: the multilap piece is the last checkpoint
    and each pass counts a lap; then "finish" with the lap count (0 on finish tracks);
  - a car kept under the sea after a restart is sent back 180 ticks (90 frames) later, +- 3 frames.
Usage: python -P tools/host/check_progress.py <dump>
"""
import re
import sys


def vec(s):
    return [int(v) for v in s.split(",")]


def main():
    events, race, settled, respawned, sunk, first_cross = [], None, None, None, None, None
    lines = open(sys.argv[1]).read().splitlines()
    for i, line in enumerate(lines):
        w = line.split()
        if not w:
            continue
        if w[0] == "race":
            race = {k: int(v) for k, v in (x.split("=") for x in w[1:])}
        elif w[0] == "event":
            events.append((int(w[1]), w[2], int(w[3])))
        elif w[0] == "settled":
            settled = dict(x.split("=") for x in w[1:])
        elif w[0] == "respawned":
            respawned = dict(x.split("=") for x in w[1:])
        elif w[0] == "sunk":
            sunk = int(w[3])
        elif w[0] == "crossed" and first_cross is None:
            first_cross = len(events)
    errors = []
    n, laps, multi = race["checkpoints"], race["laps"], race["multilap"]
    if events[:2] != [(0, "restart", 0), (90, "go", 0)]:
        errors.append("countdown: %s" % events[:2])
    st, pos = vec(settled["start"]), vec(settled["pos"])
    if int(settled["wheels"]) != 4 or int(settled["up"]) < 4055 or abs(pos[0] - st[0]) > 410 or \
            abs(pos[2] - st[2]) > 410 or not st[1] <= pos[1] <= st[1] + 3 * 4096:
        errors.append("settle: %s" % settled)

    exp = [] if multi else [("missed", n)]
    cps = n - 1 if multi else n
    for lap in range(laps):
        for c in range(cps):
            exp.append(("checkpoint", c))
            if lap == 0 and c == 0:
                exp.append(("respawn", 1))
        if multi and lap < laps - 1:
            exp += [("checkpoint", n - 1), ("lap", lap + 1)]
    exp.append(("finish", laps if multi else 0))
    exp.append(("restart", 0))
    exp.append(("go", 0))
    exp.append(("restart", 0))
    got = [(e[1], e[2]) for e in events[2:]]
    if got != exp:
        errors.append("events %s, expected %s" % (got, exp))
    rp, re_ = vec(respawned["pos"]), vec(respawned["expected"])
    if rp[0] != re_[0] or rp[2] != re_[2] or not re_[1] <= rp[1] <= re_[1] + 3 * 4096:
        errors.append("respawn at %s, expected over %s" % (rp, re_))
    last = events[-1][0]
    if not sunk + 87 <= last <= sunk + 93:
        errors.append("sea respawn at frame %d, sinking from %d" % (last, sunk))
    for e in errors:
        print("ERROR", e)
    print("%d checkpoints, %d laps, %d events, %d errors" % (n, laps, len(events), len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

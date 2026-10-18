"""Convert an FCEUX .fm2 movie to a nesrecomp cycle-host --input schedule.

    python tools/fm2_to_input.py movie.fm2 out.txt [--offset N]

fm2 input lines are |commands|RLDUTSBA|port1|port2| with one line per frame
from power-on. The host schedule is "<frame> <buttons>" (held until the next
step), so only changes are written. --offset shifts every frame (the host
and FCEUX can disagree by a frame about when a game first polls input).
"""
import argparse
import sys

# fm2 column order is R L D U T(start) S(select) B A
FM2_BUTTONS = ["RIGHT", "LEFT", "DOWN", "UP", "START", "SELECT", "B", "A"]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("fm2")
    ap.add_argument("out")
    ap.add_argument("--offset", type=int, default=0)
    args = ap.parse_args()

    frame = 0
    last = None
    steps = []
    with open(args.fm2, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.startswith("|"):
                continue
            fields = line.rstrip("\r\n").split("|")
            cmd = int(fields[1] or 0)
            if cmd & 0x3:
                print(f"warning: reset command {cmd} at fm2 frame {frame} ignored", file=sys.stderr)
            pad = fields[2]
            held = [name for name, ch in zip(FM2_BUTTONS, pad) if ch not in ".  "]
            if held != last:
                steps.append((frame + args.offset, "+".join(held) if held else "-"))
                last = held
            frame += 1

    with open(args.out, "w", encoding="ascii") as f:
        f.write(f"# converted from {args.fm2} ({frame} frames, offset {args.offset})\n")
        for fr, btn in steps:
            if fr >= 0:
                f.write(f"{fr} {btn}\n")
    print(f"{frame} frames, {len(steps)} input changes -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

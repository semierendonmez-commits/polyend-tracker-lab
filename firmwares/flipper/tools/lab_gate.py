#!/usr/bin/env python3
"""polyend-tracker-lab CPU gate (LAB_GATE.md).

Reads the output of `make -C tests lab-bench-m4` and fails when any scenario's
worst block is over its limit, or when the bench printed no scenario at all.
"""
import re
import sys

LINE = re.compile(r"^lab\.(\S+)\s+avg\s+(\d+)\s+worst\s+(\d+)\s+M4 insn per block, limit (\d+)")


def main(path):
    rows, fails = 0, []
    with open(path) as f:
        for line in f:
            m = LINE.match(line.strip())
            if not m:
                continue
            rows += 1
            name, avg, worst, limit = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4))
            pct = 100.0 * worst / limit
            status = "ok  " if worst <= limit else "FAIL"
            print(f"lab-gate: {status} {name:14s} avg {avg:7d} worst {worst:7d} insn ({pct:5.1f} % of limit {limit})")
            if worst > limit:
                fails.append(name)
    if not rows:
        print("lab-gate: FAIL no lab.* lines in the bench output")
        return 1
    if fails:
        print("lab-gate: FAIL over the CPU limit: " + ", ".join(fails))
        return 1
    print("lab-gate: CPU PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))

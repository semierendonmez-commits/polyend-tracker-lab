#!/usr/bin/env python3
"""Bring upstream OpenTracker changes into every lab firmware tree.

Each firmware under firmwares/ is a full OpenTracker tree (upstream v2 plus
the lab Shell and the workstation). This applies the upstream diff BASE..NEW
to each tree with a three-way merge and resolves the conflicts that every
lab tree has in the same places:

  Makefile                 keep the lab build (LAB_FW, lab targets) and add
                           the new upstream -D flags to DEFS
  src/app_main.cpp         keep the lab glue (it starts the Shell, not the
                           tracker App); ported upstream glue is added by hand
  tests/perf_baseline.txt  take upstream's; re-record with the lab toolchain
                           afterwards if the stock perf test is used

Anything else that conflicts is listed and left with conflict markers.

Usage (from the repo root):
  tools/upstream_merge.py <upstream-clone> <base> <new> [firmware ...]

  <upstream-clone>  a clone of sandlbn/polyend-tracker-re with <new> fetched
  <base>            the upstream commit the trees are on now (see LAB.md)
  <new>             the upstream commit to move to (e.g. origin/v2)

The upstream README.md is skipped (lab trees have none). After a merge,
delete each firmware's build/sim (the simulator makefile does not track
header dependencies) and run `make lab-gate` in every firmware.
"""
import os
import re
import subprocess
import sys

CONFLICT = re.compile(r"<<<<<<< ours\n(.*?)=======\n(.*?)>>>>>>> theirs\n", re.S)


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def resolve_makefile(ours, theirs):
    # The DEFS line: keep ours (it has -DLAB_FW_NAME) plus any -D flag that is new upstream.
    if "-DOT_SDRAM_CACHE" in ours and "-DOT_SDRAM_CACHE" in theirs:
        have = set(re.findall(r"-D\w+=", ours))
        new = [f for f in re.findall(r"(-D\w+=\$\(\w+\))", theirs) if f.split("=")[0] + "=" not in have]
        if new:
            ours = ours.replace("-DOT_FONTS_COMPRESSED=1", " ".join(new) + " -DOT_FONTS_COMPRESSED=1", 1)
        return ours
    return None


RULES = {
    "Makefile": resolve_makefile,
    "src/app_main.cpp": lambda o, t: o,
    "tests/perf_baseline.txt": lambda o, t: t,
}


def resolve(path, rule):
    s = open(path).read()
    left = []

    def rep(m):
        r = rule(m.group(1), m.group(2))
        if r is None:
            left.append(m.group(0))
            return m.group(0)
        return r

    open(path, "w").write(CONFLICT.sub(rep, s))
    return not left


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    up, base, new = sys.argv[1:4]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    fws = sys.argv[4:] or sorted(os.listdir(os.path.join(root, "firmwares")))
    patch = run(["git", "-C", up, "diff", "--binary", base, new, "--", ".", ":(exclude)README.md"]).stdout
    if not patch:
        print("no upstream changes")
        return 0
    bad = False
    for fw in fws:
        d = f"firmwares/{fw}"
        r = run(["git", "apply", "-3", f"--directory={d}"], input=patch, cwd=root)
        conflicts = re.findall(r"Applied patch to '([^']+)' with conflicts", r.stderr)
        errors = [l for l in r.stderr.splitlines() if l.startswith("error:")]
        if errors and not conflicts:
            print(f"{fw}: not applied\n  " + "\n  ".join(errors))
            bad = True
            continue
        unresolved = []
        for c in conflicts:
            rel = c[len(d) + 1:]
            rule = RULES.get(rel)
            if not (rule and resolve(os.path.join(root, c), rule)):
                unresolved.append(rel)
            else:
                run(["git", "add", c], cwd=root)
        state = "merged" if not unresolved else "CONFLICTS: " + ", ".join(unresolved)
        print(f"{fw}: {state}")
        bad |= bool(unresolved)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

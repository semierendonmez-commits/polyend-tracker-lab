#!/bin/sh
# Copy LABKIT v2 into one firmware tree:
#   tools/sync_labkit.sh <firmware-dir> [--demo]
#
# Copies src/lab/* (the Shell with LABKIT), sim/src/lab_main.cpp (simulator
# script commands), tests/lab/lab_bench.cpp (CPU bench with the LABKIT block)
# and tests/lab/labkit_test.cpp (host tests). The firmware's src/ws is never
# touched, except with --demo, which also installs the demo workstation
# (src/ws/workstation.cpp, the starting point of a new firmware) and its
# simulator check / shots scripts.
set -e
KIT=$(cd "$(dirname "$0")/.." && pwd)
FW=$1
[ -n "$FW" ] && [ -d "$FW/src/lab" ] || { echo "usage: $0 <firmware-dir> [--demo]  (a tree with src/lab)"; exit 2; }
FW=$(cd "$FW" && pwd)
case "$FW" in "$KIT"*) echo "refusing to sync into the labkit itself"; exit 2;; esac
cp "$KIT"/src/lab/*.h "$KIT"/src/lab/*.cpp "$FW/src/lab/"
cp "$KIT/sim/src/lab_main.cpp" "$FW/sim/src/lab_main.cpp"
mkdir -p "$FW/tests/lab"
cp "$KIT/tests/lab/lab_bench.cpp" "$KIT/tests/lab/labkit_test.cpp" "$FW/tests/lab/"
if [ "$2" = "--demo" ]; then
    rm -f "$FW"/src/ws/*.cpp "$FW"/src/ws/*.h
    cp "$KIT"/src/ws/* "$FW/src/ws/"
    mkdir -p "$FW/sim/lab"
    cp "$KIT/sim/lab/check.txt" "$KIT/sim/lab/shots.txt" "$FW/sim/lab/"
fi
# The simulator makefile does not track header dependencies for src/lab and
# src/ws objects (no -include of their .d files): objects compiled against the
# old lab.h would keep the old Workstation vtable and crash. Drop them.
rm -rf "$FW/build/sim/obj/lab" "$FW/build/sim/obj/ws" "$FW/build/sim/obj/sim/lab_main.o" "$FW/build/sim/lab-sim"
echo "labkit synced into $FW$([ "$2" = "--demo" ] && echo ' (with the demo workstation)')"

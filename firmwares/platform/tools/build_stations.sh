#!/bin/sh
# PLATFORM Phase B preview: build the finished sibling workstations as station
# apps (read-only use of ../<fw>/src). Output: build/stations/*.papp + *.so.
#   sh tools/build_stations.sh [fw ...]     (default: every known station that exists)
set -e
cd "$(dirname "$0")/.."
ENG=$(cd src && ls core/audio/engines/*.cpp | tr '\n' ' ')
build() {   # fw NAME MEMKB EXTRA FW_EXTRA
    [ -d "../$1/src/ws" ] || { echo "skip $2 (../$1 missing)"; return 0; }
    make -s -f sdk/station/station.mk FW="../$1" NAME="$2" MEMKB="$3" EXTRA="$4" FW_EXTRA="$5"
}
want() { [ -z "$ARGS" ] && return 0; for w in $ARGS; do [ "$w" = "$1" ] && return 0; done; return 1; }
ARGS="$*"
# MEMKB: the workstation's own arena use = its lab-bench "arena" figure minus the
# Shell's recorder rings (1,722 KB), rounded up (+ ~1 %).
want flipper    && build flipper    FLIPPER    6420  ""
want graf       && build graf       GRAF       10820 ""
want bulut      && build bulut      BULUT      10880 "core/audio/effects.cpp core/audio/audio_tables.cpp"
want datamatics && build datamatics DATAMATICS 6540  ""
want konsol     && build konsol     KONSOL     12440 "$ENG core/audio/synth_params.cpp core/audio/audio_tables.cpp core/audio/effects.cpp core/formats/patch_pb.cpp core/ui/screen_synth_tables.cpp core/ui/notes.cpp core/model/fx.cpp" "core/gen/*.cpp"
ls -l build/stations/*.papp

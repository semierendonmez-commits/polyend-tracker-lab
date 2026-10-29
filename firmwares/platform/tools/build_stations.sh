#!/bin/sh
# PLATFORM: build the sibling workstations as station apps against PLATFORM's
# lab/ (LABKIT v2; read-only use of ../<fw>/src). Output: build/stations/*.papp
# + *.so + *.info. One command: make stations (or this script).
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
# MEMKB: the least memory the station starts with (the loader refuses less):
# what its init() takes from the station arena ("station: arena N KB" in the
# hosted bench, make lab-gate) + ~2 %. LABKIT's own tables live in PLATFORM.
# GRAF last (its tree was the last to settle).
want konsol     && build konsol     KONSOL     12800 "$ENG core/audio/synth_params.cpp core/audio/audio_tables.cpp core/audio/effects.cpp core/formats/patch_pb.cpp core/ui/screen_synth_tables.cpp core/ui/notes.cpp core/model/fx.cpp" "core/gen/*.cpp"
want bulut      && build bulut      BULUT      10000 "core/audio/effects.cpp core/audio/audio_tables.cpp"
want datamatics && build datamatics DATAMATICS 7300  ""
want makam      && build makam      MAKAM      3950  "core/audio/effects.cpp core/audio/audio_tables.cpp core/formats/crc32.cpp"
want tilt       && build tilt       TILT       8700  ""
want flipper    && build flipper    FLIPPER    6100  ""
want graf       && build graf       GRAF       10400 ""
ls -l build/stations/*.papp

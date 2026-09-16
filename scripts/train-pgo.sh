#!/bin/sh
# Optional profile-guided optimization of the game module, trained on your play.
#
# PGO measured about +5% average frame rate across a spread of scenes. It is
# opt-in and local, because a profile cannot be shipped: it is instrumentation
# counters over the recompiled game code, so it is derived from the game itself
# and is not ours to distribute. Every player who wants it trains their own.
#
#   train    build an instrumented module and install it, then play normally
#   finish   merge what was recorded, rebuild optimized, install
#
# Train on what you actually play: a profile recorded entirely in one town
# optimizes for that town. Ten to fifteen minutes across a few areas, including
# at least one battle, is a reasonable target.
#
#   scripts/train-pgo.sh train
#   ... play, then quit ...
#   scripts/train-pgo.sh finish
set -eu

PHASE=${1:-}
if [ "$PHASE" != train ] && [ "$PHASE" != finish ]; then
    echo "usage: $0 train|finish" >&2
    exit 2
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
GAME=${GAME:-$ROOT/extracted/Pokemon-Colosseum-USA}
MODULES=${MODULES:-$ROOT/build/modules}
MODULES_60FPS=${MODULES_60FPS:-$ROOT/b60}
PGO_ROOT=${PGO_ROOT:-$ROOT/build/pgo}
PROFILE_RAW=$PGO_ROOT/raw
PROFILE_DATA=$PGO_ROOT/merged.profdata
PORT=$ROOT/lib/ModernGekko/build/moderngekko-port
PATCHES=$ROOT/patches/GC6E01-native-widescreen.csv
SUBSTITUTIONS=$ROOT/patches/GC6E01-native-substitutions.csv

[ -x "$PORT" ] || { echo "moderngekko-port is not built. Run scripts/setup-unix.sh first." >&2; exit 1; }
[ -d "$GAME" ] || { echo "Game root not found: $GAME (set GAME=<path>)" >&2; exit 1; }

# Built exactly as the shipped module is, or the profile describes different
# code: same DOL patches, same native substitutions.
base_args() {
    set -- build "$GAME" --backend c --toolchain clang --opt-level 2 --output "$1"
    while IFS=, read -r address expected replacement; do
        [ "$address" = address ] && continue
        replacement=$(printf '%s' "$replacement" | tr -d '\r')
        set -- "$@" --dol-patch "$address=$replacement"
    done <"$PATCHES"
    while IFS=, read -r guest symbol note; do
        [ "$guest" = guest ] && continue
        symbol=$(printf '%s' "$symbol" | tr -d '\r')
        [ -n "$symbol" ] && set -- "$@" --native-subst "$guest:$symbol"
    done <"$SUBSTITUTIONS"
    ARGS_RESULT="$*"
}

if [ "$PHASE" = train ]; then
    mkdir -p "$PROFILE_RAW"
    rm -f "$PROFILE_RAW"/*.profraw
    base_args "$MODULES"
    # shellcheck disable=SC2086
    "$PORT" $ARGS_RESULT --pgo-generate "$PROFILE_RAW"
    base_args "$MODULES_60FPS"
    # shellcheck disable=SC2086
    "$PORT" $ARGS_RESULT --pgo-generate "$PROFILE_RAW"
    cat <<EOF

Instrumented module installed. Now play the game normally.
It will run noticeably slower while training - that is expected.
Cover a few different areas, including at least one battle.

Launch the game with profiling enabled:

    LLVM_PROFILE_FILE='$PROFILE_RAW/colo-%p.profraw' <your launch command>

When you have played enough, quit and run:
    scripts/train-pgo.sh finish
EOF
    exit 0
fi

# finish
count=$(find "$PROFILE_RAW" -name '*.profraw' 2>/dev/null | wc -l | tr -d ' ')
if [ "$count" = 0 ]; then
    echo "No profile data in $PROFILE_RAW." >&2
    echo "Run 'train', play the game with LLVM_PROFILE_FILE set, then try again." >&2
    exit 1
fi
echo "Merging $count profile file(s)."

if command -v llvm-profdata >/dev/null 2>&1; then
    PROFDATA=llvm-profdata
else
    echo 'llvm-profdata not found. It ships with LLVM; add it to PATH.' >&2
    exit 1
fi
# shellcheck disable=SC2046
"$PROFDATA" merge -output="$PROFILE_DATA" $(find "$PROFILE_RAW" -name '*.profraw')

base_args "$MODULES"
# shellcheck disable=SC2086
"$PORT" $ARGS_RESULT --pgo-use "$PROFILE_DATA"
base_args "$MODULES_60FPS"
# shellcheck disable=SC2086
"$PORT" $ARGS_RESULT --pgo-use "$PROFILE_DATA"

cat <<EOF

Optimized module installed. Launch the game normally - without LLVM_PROFILE_FILE.
Profile kept at $PROFILE_DATA; rerun 'finish' to rebuild from it, or 'train'
again to record fresh play.
EOF

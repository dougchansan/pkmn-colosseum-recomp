#!/usr/bin/env sh

set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: ./setup.sh /path/to/Pokemon-Colosseum-USA.iso [jobs]" >&2
    exit 2
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ISO=$(CDPATH= cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")
JOBS=${2:-4}
DOLRECOMP=$ROOT/lib/DolRecomp
MODERNGEKKO=$ROOT/lib/ModernGekko
GAME=$ROOT/extracted/Pokemon-Colosseum-USA
DOL_BUILD=$DOLRECOMP/build
MG_BUILD=$MODERNGEKKO/build
MOD_BUILD=$ROOT/build/mods
MODULES=$ROOT/build/modules
MODULES_60FPS=$ROOT/build/modules-60fps
PACKAGE=$ROOT/build/release/PokemonColosseumRecomp
PATCHES=$ROOT/patches/GC6E01-native-widescreen.csv
# Guest functions replaced by native routines in the GX runtime. Kept beside
# the DOL patches and passed explicitly, because a module built without them
# is correct but materially slower and nothing else would flag the omission.
SUBSTITUTIONS=$ROOT/patches/GC6E01-native-substitutions.csv
DOLRECOMP_BASE=fa0cf619e8d7eb8cba7eaf55267a12caaebb46aa
MODERNGEKKO_BASE=0514d9f03f8602809f66fc92fdca87d30e752997
GRAPHICS_BACKEND=Vulkan
if [ "$(uname -s)" = Darwin ]; then
    GRAPHICS_BACKEND=Metal
fi

for command in git cmake ninja python3; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "required build tool not found: $command" >&2
        exit 1
    }
done
[ -f "$ISO" ] || { echo "Pokemon Colosseum ISO not found: $ISO" >&2; exit 1; }
python3 "$ROOT/scripts/verify-disc.py" "$ISO"

mkdir -p "$ROOT/lib"
if [ ! -f "$DOLRECOMP/CMakeLists.txt" ]; then
    git clone https://github.com/ExpansionPak/DolRecomp.git "$DOLRECOMP"
    git -C "$DOLRECOMP" checkout --detach "$DOLRECOMP_BASE"
fi
if [ ! -f "$MODERNGEKKO/CMakeLists.txt" ]; then
    git clone https://github.com/ExpansionPak/ModernGekko.git "$MODERNGEKKO"
    git -C "$MODERNGEKKO" checkout --detach "$MODERNGEKKO_BASE"
    git -C "$MODERNGEKKO" submodule update --init --recursive
fi
sh "$ROOT/mods/host-patches/apply-host-patches.sh" "$MODERNGEKKO"

cmake -S "$DOLRECOMP" -B "$DOL_BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DDOLRECOMP_ENABLE_LLVM=OFF
cmake --build "$DOL_BUILD" --parallel "$JOBS" --target dolrecomp

if [ ! -f "$GAME/sys/main.dol" ]; then
    mkdir -p "$GAME"
    "$DOL_BUILD/dolrecomp" extract "$ISO" "$GAME"
fi
python3 "$ROOT/scripts/verify-disc.py" "$ISO" "$GAME"

cmake -S "$MODERNGEKKO" -B "$MG_BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    '-DMODERNGEKKO_FRONTEND_NAME=Pokemon Colosseum Recomp' \
    -DMODERNGEKKO_LAUNCHER_OUTPUT_NAME=PokemonColosseumRecomp \
    -DMODERNGEKKO_RUNNER_OUTPUT_NAME=moderngekko-run \
    -DMODERNGEKKO_USER_DIRECTORY_NAME=PokemonColosseumRecomp \
    -DMODERNGEKKO_REQUIRED_DISC_ID=GC6E01 \
    '-DMODERNGEKKO_DEFAULT_WINDOW_TITLE=Pokemon Colosseum Recomp' \
    -DMODERNGEKKO_LOG_FILENAME=PokemonColosseumRecomp.log \
    "-DMODERNGEKKO_DOL_PATCH_MANIFEST=$PATCHES" \
    -DMODERNGEKKO_PORTABLE_DEFAULT_GAME=ON \
    -DMODERNGEKKO_PORTABLE_USER_DIRECTORY=ON \
    -DMODERNGEKKO_GAMECUBE_CONTROLLERS=ON \
    '-DMODERNGEKKO_NATIVE_ULTRAWIDE_MOD_PACKAGE=Mods/Optional/pokemon-colosseum-ultrawide.mgm' \
    '-DMODERNGEKKO_TEXT_UPSCALE_MOD_PACKAGE=Mods/Optional/pokemon-colosseum-hd-text.mgm' \
    '-DMODERNGEKKO_MINIMAP_MOD_PACKAGE=Mods/Optional/pokemon-colosseum-minimap.mgm'
cmake --build "$MG_BUILD" --parallel "$JOBS" \
    --target moderngekko-launcher moderngekko-run moderngekko-port

cmake -S "$ROOT/mods" -B "$MOD_BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release "-DMODERNGEKKO_SOURCE_DIR=$MODERNGEKKO"
cmake --build "$MOD_BUILD" --parallel "$JOBS"

set -- build "$GAME" --backend c --toolchain clang --opt-level 2 --output "$MODULES"
while IFS=, read -r address expected replacement; do
    [ "$address" = address ] && continue
    replacement=$(printf '%s' "$replacement" | tr -d '\r')
    set -- "$@" --dol-patch "$address=$replacement"
done <"$PATCHES"
while IFS=, read -r guest symbol note; do
    [ "$guest" = guest ] && continue
    symbol=$(printf '%s' "$symbol" | tr -d '')
    [ -n "$symbol" ] && set -- "$@" --native-subst "$guest:$symbol"
done <"$SUBSTITUTIONS"
"$MG_BUILD/moderngekko-port" "$@"

set -- build "$GAME" --backend c --toolchain clang --opt-level 2 --output "$MODULES_60FPS"
while IFS=, read -r address expected replacement; do
    [ "$address" = address ] && continue
    replacement=$(printf '%s' "$replacement" | tr -d '\r')
    set -- "$@" --dol-patch "$address=$replacement"
done <"$PATCHES"
# No extra --dol-patch for the 60 FPS variant. Baking 80005D98=38600000 into
# the module contradicts the guest RAM the runner presents (that address is not
# in the DOL patch manifest), so the hash guard drops chunk
# [0x800055E0,0x800095E0) to the interpreter for the session. The instruction is
# never executed anyway; 60 FPS comes from the runner's sixty_fps switch.
while IFS=, read -r guest symbol note; do
    [ "$guest" = guest ] && continue
    symbol=$(printf '%s' "$symbol" | tr -d '')
    [ -n "$symbol" ] && set -- "$@" --native-subst "$guest:$symbol"
done <"$SUBSTITUTIONS"
"$MG_BUILD/moderngekko-port" "$@"

MODULE=$(sed -e 's/\r$//' "$MODULES/GC6E01/active-module-c.txt")
case "$MODULE" in /*) ;; *) MODULE=$ROOT/$MODULE ;; esac
[ -f "$MODULE" ] || { echo "compiled C module not found: $MODULE" >&2; exit 1; }
MODULE_60FPS=$(sed -e 's/\r$//' "$MODULES_60FPS/GC6E01/active-module-c.txt")
case "$MODULE_60FPS" in /*) ;; *) MODULE_60FPS=$ROOT/$MODULE_60FPS ;; esac
[ -f "$MODULE_60FPS" ] || {
    echo "compiled 60 FPS C module not found: $MODULE_60FPS" >&2
    exit 1
}

mkdir -p "$PACKAGE/User/GameSettings" "$PACKAGE/Mods/Optional"
find "$PACKAGE" -mindepth 1 -maxdepth 1 ! -name User -exec rm -rf {} +
case "$(uname -s)" in
    Darwin) LAUNCHER=$MG_BUILD/PokemonColosseumRecomp; MODLIB=mod.dylib; MODULE_SUFFIX=.dylib ;;
    *) LAUNCHER=$MG_BUILD/PokemonColosseumRecomp; MODLIB=mod.so; MODULE_SUFFIX=.so ;;
esac
cp "$LAUNCHER" "$PACKAGE/"
cp "$MG_BUILD/moderngekko-run" "$PACKAGE/"
cp "$MODULE" "$PACKAGE/$(basename -- "$MODULE")"
cp "$MODULE_60FPS" "$PACKAGE/gGC6E01_recomp_60fps$MODULE_SUFFIX"
cp -R "$MG_BUILD/Sys" "$MG_BUILD/patches" "$PACKAGE/"
cp "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_NOTICES.md" "$ROOT/VERSION" "$PACKAGE/"
for source in "$MOD_BUILD"/packages/*.mgm; do
    target=$PACKAGE/Mods/Optional/$(basename -- "$source")
    mkdir -p "$target"
    cp "$source/$MODLIB" "$target/"
    cp -R "$source/host-patches" "$target/"
done

if [ ! -f "$PACKAGE/User/config.ini" ]; then
    cat >"$PACKAGE/User/config.ini" <<EOF
[Video]
resolution=1280x720
backend=$GRAPHICS_BACKEND
widescreen_hack=false
aspect_ratio=1
ultrawide_efb_scale=5
text_upscale=false
minimap_high_contrast=false
fullscreen=false
show_fps_in_title=true
[QualityOfLife]
sixty_fps=false
fast_forward=false
fast_forward_multiplier=2
EOF
fi
if [ ! -f "$PACKAGE/User/GameSettings/GC6E01.ini" ]; then
    printf '%s\n' '# Pokemon Colosseum stable 30 FPS profile.' \
        '[ActionReplay]' '$60 FPS (Nerdzilla)' '04005D98 38600000' \
        '[ActionReplay_Enabled]' \
        >"$PACKAGE/User/GameSettings/GC6E01.ini"
fi
printf '%s\n' "$GAME" >"$PACKAGE/default-game.txt"

echo "Setup complete. Launch: $PACKAGE/PokemonColosseumRecomp"

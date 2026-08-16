#!/usr/bin/env sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /path/to/ModernGekko" >&2
    exit 2
fi

BUNDLE_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MODERNGEKKO=$(CDPATH= cd -- "$1" && pwd)
RECOMPCORE=$MODERNGEKKO/vendor/dolphin
MODERN_BASE=884c20505d7179160f8bb01d9db0f723c53b09cb
RECOMP_BASE=13e492094902644b0d113c586300d358640f9e19
MODERN_SHA=defcee921087722f6543aae0076b863b565c4976d63eb4fa78ceecb434f9ec8c
RECOMP_SHA=09138c7c4101b6803adc6432ce89bed1796770316cb76002639de117ea0e961c

hash_file()
{
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

[ "$(hash_file "$BUNDLE_ROOT/moderngekko.patch")" = "$MODERN_SHA" ] || {
    echo "ModernGekko patch checksum mismatch." >&2
    exit 1
}
[ "$(hash_file "$BUNDLE_ROOT/recompcore.patch")" = "$RECOMP_SHA" ] || {
    echo "RecompCore patch checksum mismatch." >&2
    exit 1
}

for repository in "$MODERNGEKKO" "$RECOMPCORE"; do
    git -C "$repository" rev-parse --is-inside-work-tree >/dev/null
done
git -C "$MODERNGEKKO" merge-base --is-ancestor "$MODERN_BASE" HEAD
git -C "$RECOMPCORE" merge-base --is-ancestor "$RECOMP_BASE" HEAD

apply_one()
{
    repository=$1
    patch=$2
    component=$3
    ignore_submodules=${4:-no}

    if git -C "$repository" apply --reverse --check "$patch" >/dev/null 2>&1; then
        echo "$component host patch is already applied."
        return
    fi
    if [ "$ignore_submodules" = yes ]; then
        status=$(git -C "$repository" status --porcelain --untracked-files=no --ignore-submodules=dirty)
    else
        status=$(git -C "$repository" status --porcelain --untracked-files=no)
    fi
    if [ -n "$status" ]; then
        echo "$component has tracked changes; refusing to mix them with the release patch." >&2
        exit 1
    fi
    git -C "$repository" apply --check "$patch"
    git -C "$repository" apply "$patch"
    echo "Applied $component host patch."
}

apply_one "$RECOMPCORE" "$BUNDLE_ROOT/recompcore.patch" RecompCore
apply_one "$MODERNGEKKO" "$BUNDLE_ROOT/moderngekko.patch" ModernGekko yes

#!/usr/bin/env sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /path/to/ModernGekko" >&2
    exit 2
fi

BUNDLE_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MODERNGEKKO=$(CDPATH= cd -- "$1" && pwd)
RECOMPCORE=$MODERNGEKKO/vendor/dolphin
MODERN_BASE=0514d9f03f8602809f66fc92fdca87d30e752997
RECOMP_BASE=13e492094902644b0d113c586300d358640f9e19
# DolRecomp is a submodule of RecompCore, and carries the codegen changes
# (native substitution of guest library functions).
DOLRECOMP=$RECOMPCORE/DolRecomp
DOLRECOMP_BASE=fa0cf619e8d7eb8cba7eaf55267a12caaebb46aa
MODERN_SHA=a4b7e1cde3355009adce07b3e91cb15e3a30038b12e49b9d919371e49bcc1e8e
DOLRECOMP_SHA=85e4d543523bfde292a52eb8424bc6a44bd0f11407697e78c0b7422b48521855
RECOMP_SHA=e7567cb01a9d872f766ebfc96e62aff25ae323e368c7b5c78ebdeca0fdac8669

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
[ "$(hash_file "$BUNDLE_ROOT/dolrecomp.patch")" = "$DOLRECOMP_SHA" ] || {
    echo "DolRecomp patch checksum mismatch." >&2
    exit 1
}

for repository in "$MODERNGEKKO" "$RECOMPCORE" "$DOLRECOMP"; do
    git -C "$repository" rev-parse --is-inside-work-tree >/dev/null
done
git -C "$MODERNGEKKO" merge-base --is-ancestor "$MODERN_BASE" HEAD
git -C "$RECOMPCORE" merge-base --is-ancestor "$RECOMP_BASE" HEAD
git -C "$DOLRECOMP" merge-base --is-ancestor "$DOLRECOMP_BASE" HEAD

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

apply_one "$DOLRECOMP" "$BUNDLE_ROOT/dolrecomp.patch" DolRecomp
apply_one "$RECOMPCORE" "$BUNDLE_ROOT/recompcore.patch" RecompCore yes
apply_one "$MODERNGEKKO" "$BUNDLE_ROOT/moderngekko.patch" ModernGekko yes

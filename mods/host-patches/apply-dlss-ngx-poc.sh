#!/usr/bin/env sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /path/to/ModernGekko" >&2
    exit 2
fi

MODERNGEKKO=$(CDPATH= cd -- "$1" && pwd)
RECOMPCORE=$MODERNGEKKO/vendor/dolphin

# The release host-patch stack is based on this RecompCore revision. The Vulkan
# files touched by the NGX proof of concept are unchanged between this revision
# and the newer renderer fork, so we can apply only those paths without
# disturbing Colosseum's existing CPU/runtime patches.
RECOMP_BASE=13e492094902644b0d113c586300d358640f9e19
NGX_COMMIT=b30227a1bca5239666fe18b4d346e45f3d9712de
NGX_REMOTE=https://github.com/dougchansan/RecompCore-ModernGekko.git
NGX_BRANCH=feature/dlss-ngx-vulkan-poc

for command in git mktemp; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "required command not found: $command" >&2
        exit 1
    }
done

git -C "$RECOMPCORE" rev-parse --is-inside-work-tree >/dev/null
git -C "$RECOMPCORE" merge-base --is-ancestor "$RECOMP_BASE" HEAD

# Fetch the public proof-of-concept branch, then pin the exact reviewed commit.
# Fetching the branch (rather than relying on an unadvertised SHA fetch) keeps
# this reliable even as the branch advances.
git -C "$RECOMPCORE" fetch --quiet --no-tags "$NGX_REMOTE" "$NGX_BRANCH"
git -C "$RECOMPCORE" cat-file -e "$NGX_COMMIT^{commit}"

PATCH_FILE=$(mktemp)
trap 'rm -f "$PATCH_FILE"' EXIT HUP INT TERM

git -C "$RECOMPCORE" diff --binary "$RECOMP_BASE" "$NGX_COMMIT" -- \
    Source/Core/VideoBackends/Vulkan/CMakeLists.txt \
    Source/Core/VideoBackends/Vulkan/VKMain.cpp \
    Source/Core/VideoBackends/Vulkan/ModernGekkoNGX.cpp \
    Source/Core/VideoBackends/Vulkan/ModernGekkoNGX.h \
    >"$PATCH_FILE"

[ -s "$PATCH_FILE" ] || {
    echo "NGX overlay diff is unexpectedly empty." >&2
    exit 1
}

if git -C "$RECOMPCORE" apply --reverse --check "$PATCH_FILE" >/dev/null 2>&1; then
    echo "DLSS/NGX Vulkan proof-of-concept overlay is already applied."
    exit 0
fi

git -C "$RECOMPCORE" apply --check "$PATCH_FILE"
git -C "$RECOMPCORE" apply "$PATCH_FILE"
echo "Applied pinned DLSS/NGX Vulkan proof-of-concept overlay ($NGX_COMMIT)."

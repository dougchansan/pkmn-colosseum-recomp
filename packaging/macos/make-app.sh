#!/bin/sh
# make-app.sh <package-dir> <version>: adds PokemonColosseumRecomp.app to a
# macOS package, next to the launcher it starts (see launch.c), and signs it
# ad hoc.
set -eu
dest="$1"
version="$2"
here="$(cd "$(dirname "$0")" && pwd)"
app="$dest/PokemonColosseumRecomp.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS"
clang -O2 -Wall -mmacosx-version-min=12.0 -o "$app/Contents/MacOS/launch" "$here/launch.c" \
  -framework CoreFoundation
sed "s/@VERSION@/$version/g" "$here/Info.plist" > "$app/Contents/Info.plist"
printf 'APPL????' > "$app/Contents/PkgInfo"
codesign --force --sign - "$app"
plutil -lint "$app/Contents/Info.plist"

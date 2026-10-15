#!/bin/sh
# Downloads Sparkle, the updater a release bundles, into <dir>: its
# Sparkle.framework, its LICENSE, and the tools that make and sign the update
# feed (bin/generate_keys, bin/generate_appcast). It's Sparkle's own build,
# for macOS 12 and later. tools/build_deps.sh runs it; run it yourself for
# the tools, as when making the signing key once (README: Releases).
#   tools/sparkle.sh <dir>
set -eu
[ $# -eq 1 ] || { echo "usage: tools/sparkle.sh <dir>" >&2; exit 1; }
version=2.10.0
sha256=c2bf58aa8387266ac179357b1415d6f2635f044da8be41042af32425dae6da0c
# <dir> is replaced, so it must be new, empty, or an earlier download.
if [ -d "$1" ] && [ -n "$(ls -A "$1")" ] && [ ! -d "$1/Sparkle.framework" ]; then
    echo "sparkle.sh: $1 has other files in it; name a new folder" >&2
    exit 1
fi
rm -rf "$1"
mkdir -p "$1"
archive="$1/Sparkle-$version.tar.xz"
curl -fsSL -o "$archive" "https://github.com/sparkle-project/Sparkle/releases/download/$version/Sparkle-$version.tar.xz"
echo "$sha256  $archive" | shasum -a 256 -c - >/dev/null || {
    echo "sparkle.sh: Sparkle-$version.tar.xz doesn't match its checksum" >&2
    exit 1
}
tar -xf "$archive" -C "$1"
rm "$archive"
echo "downloaded Sparkle $version to $1"

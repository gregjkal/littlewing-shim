#!/bin/sh
# Releases LittleWing.app as version x.y.z: from a clean main that matches
# GitHub, it builds Unicorn and SDL3 (tools/build_deps.sh) and a fresh Release
# build against them in build-dist, all for macOS 26 and later, runs the
# tests, builds the app signed with LOONY_SIGN_ID, checks it runs on macOS 26,
# notarizes it, tags the commit vx.y.z, pushes the tag and makes a draft
# GitHub release with the zip. Check the draft on GitHub, then publish it.
# The profile is notarize.sh's (default loony-notary).
#   LOONY_SIGN_ID=<identity> tools/release.sh <x.y.z> [keychain profile]
set -eu
die() {
    echo "release.sh: $*" >&2
    exit 1
}
[ $# -ge 1 ] || die "usage: LOONY_SIGN_ID=<identity> tools/release.sh <x.y.z> [keychain profile]"
version=$1
profile=${2:-loony-notary}
tag=v$version
echo "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || die "the version must be x.y.z, not $version"
[ -n "${LOONY_SIGN_ID:-}" ] || die "set LOONY_SIGN_ID to your Developer ID Application identity"
cd "$(dirname "$0")/.."

git fetch -q origin
[ "$(git rev-parse --abbrev-ref HEAD)" = main ] || die "check out main first"
[ -z "$(git status --porcelain)" ] || die "the working tree has changes or untracked files"
[ "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)" ] || die "main doesn't match origin/main"
if git rev-parse -q --verify "refs/tags/$tag" >/dev/null || [ -n "$(git ls-remote --tags origin "refs/tags/$tag")" ]; then
    die "$tag already exists"
fi

build=build-dist
macos=26.0  # the oldest macOS a release runs on
rm -rf "$build"
MACOSX_DEPLOYMENT_TARGET=$macos tools/build_deps.sh "$build/deps"
PKG_CONFIG_PATH="$PWD/$build/deps/unicorn/lib/pkgconfig:$PWD/$build/deps/sdl3/lib/pkgconfig"
export PKG_CONFIG_PATH
# An empty CMAKE_PROJECT_INCLUDE, so no include from the environment or a
# cached setting (such as a local dev build's) gets into a release.
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=$macos -DCMAKE_PROJECT_INCLUDE=
cmake --build "$build"
"$build/loony_tests"
LOONY_VERSION=$version cmake --build "$build" --target app
# A local dev build marks itself with this string (see local_patch_hook in
# src/main.c); a release must not contain it.
if grep -q LOONY-LOCAL-DEV-UNLOCK "$build/LittleWing.app/Contents/MacOS/loony"; then
    die "the app contains the local dev unlock"
fi
min=$(plutil -extract LSMinimumSystemVersion raw "$build/LittleWing.app/Contents/Info.plist")
[ "$min" = "$macos" ] || die "the app needs macOS $min, not $macos"
tools/notarize.sh "$build/LittleWing.app" "$profile"
zip="$build/LittleWing-$version.zip"
mv "$build/LittleWing.zip" "$zip"

unicorn=$(pkg-config --modversion unicorn)
sdl=$(pkg-config --modversion sdl3)
# The notes go through a file: macOS's sh (bash 3.2) misreads an apostrophe
# in a here-document inside "$(...)".
notes=$(mktemp)
cat >"$notes" <<NOTES
Plays LittleWing's *Loony Labyrinth 3.0.1* and *Crystal Caliburn 3.0.1* on an Apple Silicon Mac with macOS $macos or later. This is an unofficial project, not made or endorsed by LittleWing.

**To install:** download \`LittleWing-$version.zip\`, unzip it, move \`LittleWing.app\` to Applications, and install the games as described in [Play](https://github.com/gregjkal/littlewing-shim#play). The app is notarized, so it opens without a warning. It contains no game files.

The app bundles [Unicorn $unicorn](https://github.com/unicorn-engine/unicorn/releases/tag/$unicorn) (GPLv2) and [SDL $sdl](https://github.com/libsdl-org/SDL/releases/tag/release-$sdl) (zlib). Their licenses are in \`LittleWing.app/Contents/Resources/Licenses\`.
NOTES
git tag -a "$tag" -m "LittleWing $version"
git push origin "$tag"
gh release create "$tag" "$zip" --verify-tag --draft --title "LittleWing $version" --generate-notes --notes "$(cat "$notes")"
rm -f "$notes"
echo "made a draft release of $tag: check it on GitHub, then publish it"

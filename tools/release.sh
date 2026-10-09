#!/bin/sh
# Releases LittleWing.app as version x.y.z: from a clean main that matches
# GitHub, it builds Unicorn and SDL3 (tools/build_deps.sh) and a fresh Release
# build against them in build-dist, all for macOS 26 and later, runs the
# tests, builds the app signed with LOONY_SIGN_ID, checks it runs on macOS 26,
# notarizes it, tags the commit vx.y.z, pushes the tag and makes a draft
# GitHub release with the zip. The notes are tools/release_notes.md with its
# @VERSION@, @MACOS@, @UNICORN@ and @SDL@ filled in, followed by GitHub's list
# of the pull requests since the last release. Check the draft on GitHub, then
# publish it.
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
unknown=$(grep -o '@[A-Z]*@' tools/release_notes.md | grep -vxE '@(VERSION|MACOS|UNICORN|SDL)@' || true)
[ -z "$unknown" ] || die "tools/release_notes.md has placeholders release.sh doesn't fill: $unknown"

build=build-dist
macos=26.0  # the oldest macOS a release runs on
rm -rf "$build"
MACOSX_DEPLOYMENT_TARGET=$macos tools/build_deps.sh "$build/deps"
PKG_CONFIG_PATH="$PWD/$build/deps/unicorn/lib/pkgconfig:$PWD/$build/deps/sdl3/lib/pkgconfig"
export PKG_CONFIG_PATH
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=$macos
cmake --build "$build"
"$build/loony_tests"
LOONY_VERSION=$version cmake --build "$build" --target app
min=$(plutil -extract LSMinimumSystemVersion raw "$build/LittleWing.app/Contents/Info.plist")
[ "$min" = "$macos" ] || die "the app needs macOS $min, not $macos"
tools/notarize.sh "$build/LittleWing.app" "$profile"
zip="$build/LittleWing-$version.zip"
mv "$build/LittleWing.zip" "$zip"

unicorn=$(pkg-config --modversion unicorn)
sdl=$(pkg-config --modversion sdl3)
notes=$(mktemp)
sed -e "s/@VERSION@/$version/g" -e "s/@MACOS@/$macos/g" -e "s/@UNICORN@/$unicorn/g" -e "s/@SDL@/$sdl/g" \
    tools/release_notes.md >"$notes"
git tag -a "$tag" -m "LittleWing $version"
git push origin "$tag"
gh release create "$tag" "$zip" --verify-tag --draft --title "LittleWing $version" --generate-notes --notes "$(cat "$notes")"
rm -f "$notes"
echo "made a draft release of $tag: check it on GitHub, then publish it"

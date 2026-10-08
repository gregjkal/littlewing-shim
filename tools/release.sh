#!/bin/sh
# Releases LittleWing.app as version x.y.z: from a clean main that matches
# GitHub, it builds a fresh Release build in build-dist, runs the tests, builds
# the app signed with LOONY_SIGN_ID, notarizes it, tags the commit vx.y.z,
# pushes the tag and makes a draft GitHub release with the zip. Check the
# draft on GitHub, then publish it. The profile is notarize.sh's (default
# loony-notary).
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
rm -rf "$build"
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build"
"$build/loony_tests"
LOONY_VERSION=$version cmake --build "$build" --target app
tools/notarize.sh "$build/LittleWing.app" "$profile"
zip="$build/LittleWing-$version.zip"
mv "$build/LittleWing.zip" "$zip"

unicorn=$(pkg-config --modversion unicorn)
sdl=$(pkg-config --modversion sdl3)
git tag -a "$tag" -m "LittleWing $version"
git push origin "$tag"
gh release create "$tag" "$zip" --verify-tag --draft --title "LittleWing $version" --generate-notes --notes "$(cat <<NOTES
Plays LittleWing's *Loony Labyrinth 3.0.1* and *Crystal Caliburn 3.0.1* on an Apple Silicon Mac with macOS 26 or later. This is an unofficial project, not made or endorsed by LittleWing.

**To install:** download \`LittleWing-$version.zip\`, unzip it, move \`LittleWing.app\` to Applications, and install the games as described in [Play](https://github.com/gregjkal/littlewing-shim#play). The app is notarized, so it opens without a warning. It contains no game files.

The app bundles [Unicorn $unicorn](https://github.com/unicorn-engine/unicorn/releases/tag/$unicorn) (GPLv2) and [SDL $sdl](https://github.com/libsdl-org/SDL/releases/tag/release-$sdl) (zlib). Their licenses are in \`LittleWing.app/Contents/Resources/Licenses\`.
NOTES
)"
echo "made a draft release of $tag: check it on GitHub, then publish it"

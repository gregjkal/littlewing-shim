#!/bin/sh
# Releases LittleWing.app as version x.y.z: from a clean main that matches
# GitHub, it builds Unicorn and SDL3 (tools/build_deps.sh) and a fresh Release
# build against them in build-dist, all for macOS 14 and later, runs the
# tests, builds the app signed with LOONY_SIGN_ID, checks it runs on macOS 14,
# notarizes it, and makes appcast.xml, the update feed, which offers the zip,
# signed with the Sparkle key in the keychain. Only then does it tag the
# commit vx.y.z, push the tag and make a draft GitHub release with the zip and
# the feed. The notes are tools/release_notes.md with its @VERSION@, @MACOS@,
# @UNICORN@, @SDL@ and @SPARKLE@ filled in, followed by GitHub's list of the
# pull requests since the last release, which is also the feed's notes. The
# app updates itself with Sparkle (tools/make_app.sh) and reads the latest
# release's feed, so apps see the update once the release is published.
# Check the draft on GitHub, then publish it.
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
unknown=$(grep -o '@[A-Z]*@' tools/release_notes.md | grep -vxE '@(VERSION|MACOS|UNICORN|SDL|SPARKLE)@' || true)
[ -z "$unknown" ] || die "tools/release_notes.md has placeholders release.sh doesn't fill: $unknown"
[ -s tools/sparkle_public_key.txt ] || die "tools/sparkle_public_key.txt is missing: make the Sparkle signing key (README: Releases)"
gh auth status >/dev/null 2>&1 || die "gh isn't logged in to GitHub (gh auth login)"

build=build-dist
macos=14.0  # the oldest macOS a release runs on
rm -rf "$build"
MACOSX_DEPLOYMENT_TARGET=$macos tools/build_deps.sh "$build/deps"
sparkle=$PWD/$build/deps/sparkle
key_account=littlewing-shim  # the keychain item holding the Sparkle signing key
echo "reading the Sparkle key: if macOS asks to allow generate_keys keychain access, click Always Allow"
key=$("$sparkle/bin/generate_keys" --account "$key_account" -p) || die "the keychain has no Sparkle key for $key_account (README: Releases)"
[ "$key" = "$(tr -d ' \n' <tools/sparkle_public_key.txt)" ] ||
    die "the keychain's Sparkle key isn't the one in tools/sparkle_public_key.txt, so apps would refuse the update"
PKG_CONFIG_PATH="$PWD/$build/deps/unicorn/lib/pkgconfig:$PWD/$build/deps/sdl3/lib/pkgconfig"
export PKG_CONFIG_PATH
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=$macos
cmake --build "$build"
"$build/loony_tests"
LOONY_VERSION=$version LOONY_SPARKLE=$sparkle cmake --build "$build" --target app
min=$(plutil -extract LSMinimumSystemVersion raw "$build/LittleWing.app/Contents/Info.plist")
[ "$min" = "$macos" ] || die "the app needs macOS $min, not $macos"
tools/notarize.sh "$build/LittleWing.app" "$profile"
zip="$build/LittleWing-$version.zip"
mv "$build/LittleWing.zip" "$zip"

unicorn=$(pkg-config --modversion unicorn)
sdl=$(pkg-config --modversion sdl3)
sparkle_version=$(plutil -extract CFBundleShortVersionString raw "$sparkle/Sparkle.framework/Resources/Info.plist")
# GitHub's list of the pull requests since the last release, made before the
# tag exists, so nothing is pushed until the update feed is signed.
generated=$(gh api repos/gregjkal/littlewing-shim/releases/generate-notes \
    -f tag_name="$tag" -f target_commitish="$(git rev-parse HEAD)" -q .body)
[ -n "$generated" ] || die "GitHub made no notes for $tag"

# The update feed, with GitHub's list as its notes.
updates="$build/updates"
mkdir "$updates"
cp "$zip" "$updates/"
printf '%s\n' "$generated" >"$updates/LittleWing-$version.md"
echo "signing the update feed: if macOS asks to allow generate_appcast keychain access, click Always Allow"
"$sparkle/bin/generate_appcast" --account "$key_account" --maximum-deltas 0 --embed-release-notes \
    --download-url-prefix "https://github.com/gregjkal/littlewing-shim/releases/download/$tag/" \
    --link https://github.com/gregjkal/littlewing-shim -o "$updates/appcast.xml" "$updates"
grep -q "sparkle:edSignature" "$updates/appcast.xml" || die "$updates/appcast.xml has no signature"

notes="$build/notes.md"
sed -e "s/@VERSION@/$version/g" -e "s/@MACOS@/$macos/g" -e "s/@UNICORN@/$unicorn/g" -e "s/@SDL@/$sdl/g" \
    -e "s/@SPARKLE@/$sparkle_version/g" tools/release_notes.md >"$notes"
printf '\n%s\n' "$generated" >>"$notes"
git tag -a "$tag" -m "LittleWing $version"
git push origin "$tag"
# The tag is pushed, so a rerun would stop at it: say how to finish instead.
gh release create "$tag" "$zip" "$updates/appcast.xml" --verify-tag --draft --title "LittleWing $version" \
    --notes-file "$notes" ||
    die "$tag is pushed but its release wasn't made; finish with: gh release create $tag $zip $updates/appcast.xml --verify-tag --draft --title 'LittleWing $version' --notes-file $notes"
echo "made a draft release of $tag: check it on GitHub, then publish it"

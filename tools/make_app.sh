#!/bin/sh
# Builds LittleWing.app around a loony binary: it plays the LittleWing games
# installed in /Applications, with a picker when there are two or three. The Unicorn
# and SDL3 libraries are copied into the bundle (so a Homebrew upgrade can't break
# it), the icon is made from tools/AppIcon.png, and the bundle is signed with
# the hardened runtime and the allow-jit entitlement: ad hoc, or with the
# identity in LOONY_SIGN_ID (a "Developer ID Application: ..." certificate in
# the keychain) for an app that can be notarized and given to others. Its
# version is LOONY_VERSION (x.y.z), or 0.0.0 for a build that isn't a release,
# and the oldest macOS it claims to run on is the newest any of its binaries
# needs (a release builds them all for macOS 26; see tools/release.sh).
# Resources/Licenses holds this project's license and the bundled libraries'.
#   [LOONY_SIGN_ID=<identity>] [LOONY_VERSION=<x.y.z>] tools/make_app.sh <loony binary> <output folder>
set -eu
bin=$1
out=$2
version=${LOONY_VERSION:-0.0.0}
here=$(cd "$(dirname "$0")" && pwd)
app="$out/LittleWing.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks" "$app/Contents/Resources"
cp "$bin" "$app/Contents/MacOS/loony"
# The icon: tools/AppIcon.png (1024x1024, original art) at every iconset size.
iconset=$(mktemp -d)/AppIcon.iconset
mkdir "$iconset"
for s in 16 32 128 256 512; do
    sips -z $s $s "$here/AppIcon.png" --out "$iconset/icon_${s}x${s}.png" >/dev/null
    sips -z $((s * 2)) $((s * 2)) "$here/AppIcon.png" --out "$iconset/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$iconset" -o "$app/Contents/Resources/AppIcon.icns"
rm -rf "$(dirname "$iconset")"
cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>loony</string>
	<key>CFBundleIconFile</key><string>AppIcon</string>
	<key>CFBundleIdentifier</key><string>com.gregkaleka.littlewing-shim</string>
	<key>CFBundleName</key><string>LittleWing</string>
	<key>CFBundleDisplayName</key><string>LittleWing</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>$version</string>
	<key>CFBundleVersion</key><string>$version</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>LSApplicationCategoryType</key><string>public.app-category.arcade-games</string>
	<key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
# Copy each library the binary links from outside the system (Homebrew's, or
# tools/build_deps.sh's, found through the binary's search paths) and point
# the binary at the copy, with the license files of the package it came from.
licenses="$app/Contents/Resources/Licenses"
mkdir -p "$licenses"
cp "$here/../LICENSE" "$licenses/littlewing-shim-LICENSE"
rpaths=$(otool -l "$bin" | awk '/cmd LC_RPATH/ {getline; getline; print $2}')
for dep in $(otool -L "$bin" | tail -n +2 | awk '{print $1}'); do  # paths without spaces
    case "$dep" in
    /usr/lib/* | /System/*) continue ;;
    @rpath/*)
        lib=
        for rp in $rpaths; do
            if [ -f "$rp/${dep#@rpath/}" ]; then
                lib="$rp/${dep#@rpath/}"
                break
            fi
        done
        [ -n "$lib" ] || { echo "make_app.sh: $dep isn't in any of $bin's search paths" >&2; exit 1; } ;;
    *) lib=$dep ;;
    esac
    name=$(basename "$dep")
    cp "$lib" "$app/Contents/Frameworks/$name"
    pkg=$(cd "$(dirname "$lib")/.." && pwd -P)  # a Homebrew Cellar folder, or build_deps.sh's
    short=$(echo "$name" | sed 's/^lib//; s/\..*//')  # libSDL3.0.dylib: SDL3
    for f in "$pkg"/COPYING* "$pkg"/LICENSE*; do
        if [ -f "$f" ]; then
            cp "$f" "$licenses/$short-$(basename "$f")"
        fi
    done
    chmod u+w "$app/Contents/Frameworks/$name"
    install_name_tool -id "@rpath/$name" "$app/Contents/Frameworks/$name"
    if [ "$dep" != "@rpath/$name" ]; then
        install_name_tool -change "$dep" "@rpath/$name" "$app/Contents/MacOS/loony"
    fi
done
# Search only Frameworks (the link adds the libraries' folders).
for rp in $(otool -l "$app/Contents/MacOS/loony" | awk '/cmd LC_RPATH/ {getline; getline; print $2}'); do
    install_name_tool -delete_rpath "$rp" "$app/Contents/MacOS/loony"
done
install_name_tool -add_rpath "@executable_path/../Frameworks" "$app/Contents/MacOS/loony"
# Self-contained: every library is the system's or in Frameworks, and the
# only search path is Frameworks. (A sanitizer build fails here: it needs
# the compiler's runtime library.)
bad=0
for f in "$app/Contents/MacOS/loony" "$app"/Contents/Frameworks/*.dylib; do
    for dep in $(otool -L "$f" | tail -n +2 | awk '{print $1}'); do
        case "$dep" in
        /usr/lib/* | /System/*) ;;
        @rpath/*) [ -f "$app/Contents/Frameworks/${dep#@rpath/}" ] || { echo "make_app.sh: $f needs $dep, which isn't bundled" >&2; bad=1; } ;;
        *) echo "make_app.sh: $f needs $dep, outside the bundle" >&2; bad=1 ;;
        esac
    done
    for rp in $(otool -l "$f" | awk '/cmd LC_RPATH/ {getline; getline; print $2}'); do
        [ "$rp" = "@executable_path/../Frameworks" ] || { echo "make_app.sh: $f searches $rp" >&2; bad=1; }
    done
done
if [ "$bad" != 0 ]; then
    echo "make_app.sh: the bundle isn't self-contained (build it from a Release build)" >&2
    exit 1
fi
# The oldest macOS the app runs on is the newest that any of its binaries needs.
min=$(for f in "$app/Contents/MacOS/loony" "$app"/Contents/Frameworks/*.dylib; do
    vtool -show-build "$f" | awk '/minos/ {print $2}'
done | sort -t . -k 1,1n -k 2,2n -k 3,3n | tail -1)
plutil -insert LSMinimumSystemVersion -string "$min" "$app/Contents/Info.plist"
# A Developer ID signature needs a secure timestamp (fetched from Apple) to be notarized.
sign=${LOONY_SIGN_ID:--}
if [ "$sign" = - ]; then set -- ; else set -- --timestamp; fi
for lib in "$app"/Contents/Frameworks/*.dylib; do
    codesign --force --sign "$sign" "$@" --options runtime "$lib"
done
codesign --force --sign "$sign" "$@" --options runtime --entitlements "$here/loony.entitlements" "$app"
codesign --verify --deep --strict "$app"
echo "built $app (for macOS $min and later)"

#!/bin/sh
# Builds "Loony Labyrinth.app" around a loony binary: the Unicorn and SDL3
# libraries are copied into the bundle (so a Homebrew upgrade can't break
# it), the icon is made from tools/AppIcon.png, and the bundle is signed ad hoc with the hardened runtime and the
# allow-jit entitlement.
#   tools/make_app.sh <loony binary> <output folder>
set -eu
bin=$1
out=$2
here=$(cd "$(dirname "$0")" && pwd)
app="$out/Loony Labyrinth.app"
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
	<key>CFBundleIdentifier</key><string>local.loony-shim</string>
	<key>CFBundleName</key><string>Loony Labyrinth</string>
	<key>CFBundleDisplayName</key><string>Loony Labyrinth</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>3.0.1</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>LSMinimumSystemVersion</key><string>13.0</string>
	<key>LSApplicationCategoryType</key><string>public.app-category.arcade-games</string>
	<key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
# Copy each Homebrew library the binary links and point the binary at the copy.
for lib in $(otool -L "$bin" | awk '/\/opt\/homebrew\// {print $1}'); do  # paths without spaces
    name=$(basename "$lib")
    cp "$lib" "$app/Contents/Frameworks/$name"
    chmod u+w "$app/Contents/Frameworks/$name"
    install_name_tool -id "@rpath/$name" "$app/Contents/Frameworks/$name"
    install_name_tool -change "$lib" "@rpath/$name" "$app/Contents/MacOS/loony"
done
# Search only Frameworks (the link adds /opt/homebrew/lib).
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
for lib in "$app"/Contents/Frameworks/*.dylib; do
    codesign --force --sign - --options runtime "$lib"
done
codesign --force --sign - --options runtime --entitlements "$here/loony.entitlements" "$app"
codesign --verify --strict "$app"
echo "built $app"

#!/bin/sh
# Builds "Loony Labyrinth.app" around a loony binary: the Unicorn and SDL3
# libraries are copied into the bundle (so a Homebrew upgrade can't break
# it), and the bundle is signed ad hoc with the hardened runtime and the
# allow-jit entitlement.
#   tools/make_app.sh <loony binary> <output folder>
set -eu
bin=$1
out=$2
here=$(cd "$(dirname "$0")" && pwd)
app="$out/Loony Labyrinth.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks"
cp "$bin" "$app/Contents/MacOS/loony"
cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>loony</string>
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
for lib in $(otool -L "$bin" | awk '/\/opt\/homebrew\// {print $1}'); do
    name=$(basename "$lib")
    cp "$lib" "$app/Contents/Frameworks/$name"
    chmod u+w "$app/Contents/Frameworks/$name"
    install_name_tool -id "@rpath/$name" "$app/Contents/Frameworks/$name"
    install_name_tool -change "$lib" "@rpath/$name" "$app/Contents/MacOS/loony"
done
install_name_tool -add_rpath "@executable_path/../Frameworks" "$app/Contents/MacOS/loony"
if otool -L "$app/Contents/MacOS/loony" "$app"/Contents/Frameworks/*.dylib | grep -q /opt/homebrew/; then
    echo "make_app.sh: the bundle still refers to Homebrew libraries" >&2
    exit 1
fi
for lib in "$app"/Contents/Frameworks/*.dylib; do
    codesign --force --sign - --options runtime "$lib"
done
codesign --force --sign - --options runtime --entitlements "$here/loony.entitlements" "$app"
codesign --verify --strict "$app"
echo "built $app"

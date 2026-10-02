#!/bin/sh
# Notarizes an app built with LOONY_SIGN_ID set, staples the ticket to it, and
# zips it for giving to others: Gatekeeper then opens it on any Mac without
# a warning, even offline. The profile holds the Apple ID credentials, saved
# once with `xcrun notarytool store-credentials <profile>`.
#   tools/notarize.sh <app> <keychain profile>
set -eu
app=$1
profile=$2
zip="$(dirname "$app")/$(basename "$app" .app).zip"
codesign -dvv "$app" 2>&1 | grep -q '^Authority=Developer ID Application' || {
    echo "notarize.sh: $app isn't signed with a Developer ID (build it with LOONY_SIGN_ID set)" >&2
    exit 1
}
rm -f "$zip"
ditto -c -k --keepParent "$app" "$zip"
xcrun notarytool submit "$zip" --keychain-profile "$profile" --wait
xcrun stapler staple "$app"
spctl --assess --type execute -vv "$app"
# Zip again, now with the ticket inside.
rm -f "$zip"
ditto -c -k --keepParent "$app" "$zip"
echo "built $zip"

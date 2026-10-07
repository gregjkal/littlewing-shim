#!/bin/bash
# Makes HD replacement art from a picture dump (LOONY_HD_DUMP), for LOONY_HD.
#
#   tools/hd_art.sh <dump dir> <art dir> [scale] [hash...]
#
# Each picture is de-dithered with a selective blur (the game's 8-bit art is
# dithered to the Mac system palette), enlarged with Lanczos and sharpened.
# The blur smooths neighbours within 15% of each other's brightness, which
# also smears red text on black and faint detail. Pictures in GENTLE get 5%
# instead: Loony Labyrinth's side panel, for CREDIT and BALL and the unlit
# display segments. Add any other picture whose small text comes out blurred.
# This is a stand-in: art from a better upscaler, or redrawn, goes in the art
# dir under the same <hash>.png name, at any size. With hashes, only those
# pictures are made. Needs ImageMagick (brew install imagemagick).
set -euo pipefail

if [ $# -lt 2 ]; then
    echo "usage: $0 <dump dir> <art dir> [scale] [hash...]" >&2
    exit 2
fi
dump=$1
art=$2
scale=${3:-4}
shift $(($# < 3 ? $# : 3))
mkdir -p "$art"

if [ $# -gt 0 ]; then
    files=()
    for h in "$@"; do files+=("$dump/$h.png"); done
else
    files=("$dump"/*.png)
fi

GENTLE=" 78c432e7 " # Loony Labyrinth's side panel

for f in "${files[@]}"; do
    name=$(basename "$f" .png)
    threshold=15
    [[ "$GENTLE" == *" $name "* ]] && threshold=5
    magick "$f" -selective-blur "0x1.5+$threshold%" -filter Lanczos -resize "$((scale * 100))%" \
        -unsharp 0x1.5+0.7 -define png:exclude-chunks=date,time "$art/$name.png"
done
echo "made ${#files[@]} pictures in $art"

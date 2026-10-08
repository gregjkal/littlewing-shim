#!/bin/sh
# Builds the libraries the app bundles, Unicorn (its PowerPC CPU only) and
# SDL3, from their release sources for macOS MACOSX_DEPLOYMENT_TARGET and
# later. Homebrew's copies need the macOS they were built for, so a release
# uses these. Each is installed in <prefix>/<name>, with its license files;
# the release build finds them through PKG_CONFIG_PATH.
#   MACOSX_DEPLOYMENT_TARGET=<x.y> tools/build_deps.sh <prefix>
set -eu
[ $# -eq 1 ] || { echo "usage: MACOSX_DEPLOYMENT_TARGET=<x.y> tools/build_deps.sh <prefix>" >&2; exit 1; }
: "${MACOSX_DEPLOYMENT_TARGET:?set it to the oldest macOS to support}"
mkdir -p "$1"
prefix=$(cd "$1" && pwd)
src="$prefix/src"
rm -rf "$src" "$prefix/unicorn" "$prefix/sdl3"
mkdir -p "$src"

# fetch <url> <sha256> <file>: downloads and checks one source archive.
fetch() {
    curl -fsSL -o "$src/$3" "$1"
    echo "$2  $src/$3" | shasum -a 256 -c - >/dev/null || {
        echo "build_deps.sh: $3 doesn't match its checksum" >&2
        exit 1
    }
    tar -xzf "$src/$3" -C "$src"
}

fetch https://github.com/unicorn-engine/unicorn/archive/refs/tags/2.1.4.tar.gz \
    ea8863f095a0136388694e5a6063afd9bb7650e30243dd6251af59c5ce5601f4 unicorn-2.1.4.tar.gz
cmake -S "$src/unicorn-2.1.4" -B "$src/build-unicorn" -DCMAKE_BUILD_TYPE=Release -DUNICORN_ARCH=ppc \
    -DUNICORN_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX="$prefix/unicorn"
cmake --build "$src/build-unicorn" -j "$(sysctl -n hw.ncpu)"
cmake --install "$src/build-unicorn"
cp "$src"/unicorn-2.1.4/COPYING* "$prefix/unicorn/"

fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz \
    7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68 SDL3-3.4.16.tar.gz
cmake -S "$src/SDL3-3.4.16" -B "$src/build-sdl3" -DCMAKE_BUILD_TYPE=Release -DSDL_TESTS=OFF \
    -DSDL_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX="$prefix/sdl3"
cmake --build "$src/build-sdl3" -j "$(sysctl -n hw.ncpu)"
cmake --install "$src/build-sdl3"
cp "$src/SDL3-3.4.16/LICENSE.txt" "$prefix/sdl3/"

echo "built Unicorn 2.1.4 and SDL 3.4.16 for macOS $MACOSX_DEPLOYMENT_TARGET in $prefix"

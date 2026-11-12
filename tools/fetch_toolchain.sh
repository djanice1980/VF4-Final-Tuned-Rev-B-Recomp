#!/usr/bin/env bash
# Download the pinned cross toolchain and SDL3 source into toolchain/,
# check their SHA-256, and build SDL3 for Windows x86-64.
#
# usage: tools/fetch_toolchain.sh
#
# needs: curl, tar, xz, cmake, ninja (pacman: curl tar xz cmake ninja)
#
# Result:
#   toolchain/llvm-mingw/             clang, lld, mingw-w64 headers (UCRT)
#   toolchain/sdl3-win64/             SDL3 built with that clang (bin/SDL3.dll, lib, include, cmake)
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
tc=$root/toolchain
mkdir -p "$tc/dl"

LLVM_MINGW_VER=20261104
LLVM_MINGW_FILE=llvm-mingw-$LLVM_MINGW_VER-ucrt-ubuntu-22.04-x86_64.tar.xz
LLVM_MINGW_SHA256=919380403de87dca3c581c2053230cd298af5e891286628ba0613b7fed370926
LLVM_MINGW_URL=https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW_VER/$LLVM_MINGW_FILE

SDL3_VER=3.4.20
SDL3_FILE=SDL3-$SDL3_VER.tar.gz
SDL3_SHA256=f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f
SDL3_URL=https://github.com/libsdl-org/SDL/releases/download/release-$SDL3_VER/$SDL3_FILE

fetch() { # url file sha256
    local f=$tc/dl/$2
    if [[ ! -f $f ]] || ! echo "$3  $f" | sha256sum -c --status; then
        echo "downloading $2"
        curl -fL --retry 3 -o "$f.part" "$1"
        mv "$f.part" "$f"
    fi
    echo "$3  $f" | sha256sum -c --quiet
}

fetch "$LLVM_MINGW_URL" "$LLVM_MINGW_FILE" "$LLVM_MINGW_SHA256"
fetch "$SDL3_URL" "$SDL3_FILE" "$SDL3_SHA256"

stamp=$tc/llvm-mingw/.version
if [[ $(cat "$stamp" 2>/dev/null) != "$LLVM_MINGW_VER" ]]; then
    rm -rf "$tc/llvm-mingw"
    mkdir -p "$tc/llvm-mingw"
    tar -xJf "$tc/dl/$LLVM_MINGW_FILE" -C "$tc/llvm-mingw" --strip-components=1
    echo "$LLVM_MINGW_VER" > "$stamp"
fi

stamp=$tc/sdl3-win64/.version
want="$SDL3_VER llvm-mingw-$LLVM_MINGW_VER"
if [[ $(cat "$stamp" 2>/dev/null) != "$want" ]]; then
    rm -rf "$tc/sdl3-src" "$tc/sdl3-build" "$tc/sdl3-win64"
    mkdir -p "$tc/sdl3-src"
    tar -xzf "$tc/dl/$SDL3_FILE" -C "$tc/sdl3-src" --strip-components=1
    cmake -S "$tc/sdl3-src" -B "$tc/sdl3-build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$root/cmake/llvm-mingw-x86_64.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$tc/sdl3-win64" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF \
        -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
    cmake --build "$tc/sdl3-build"
    cmake --install "$tc/sdl3-build"
    echo "$want" > "$stamp"
fi

echo "toolchain ready: $tc/llvm-mingw, $tc/sdl3-win64"

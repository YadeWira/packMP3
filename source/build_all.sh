#!/usr/bin/env bash
# build_all.sh — builds packMP3 for Linux x64, Windows x86, and Windows x64
#
# Requirements: clang++ (or g++), mingw-w64
#   Ubuntu/Debian : sudo apt install mingw-w64
#   Arch          : sudo pacman -S mingw-w64-gcc
#   macOS         : brew install mingw-w64

set -e
cd "$(dirname "$0")"

SRC="aricoder.cpp bitops.cpp huffmp3.cpp packmp3.cpp"
CFLAGS="-I. -O3 -Wall -Wextra -funroll-loops -ffast-math -fomit-frame-pointer -std=c++17"
OUT="bin"

mkdir -p "$OUT"

ok()   { echo "[OK]  $*"; }
fail() { echo "[!!]  $*"; exit 1; }

# --- Linux x64 ---------------------------------------------------------------

echo ""
echo "==> Linux x64"
if command -v clang++ &>/dev/null; then
    CXX="clang++"
elif command -v g++ &>/dev/null; then
    CXX="g++"
else
    fail "No C++ compiler found (clang++ or g++ required)"
fi

$CXX $CFLAGS -DUNIX \
    -o "$OUT/packMP3_linux_x64" \
    $SRC \
    && ok "bin/packMP3_linux_x64"

# --- Windows x64 -------------------------------------------------------------

echo ""
echo "==> Windows x64"
WIN64="x86_64-w64-mingw32-g++"
if ! command -v $WIN64 &>/dev/null; then
    echo "    [--] $WIN64 not found, skipping Windows x64"
    echo "         Install with: sudo apt install mingw-w64"
else
    $WIN64 $CFLAGS \
        -o "$OUT/packMP3_win_x64.exe" \
        $SRC icons.res \
        -static -static-libgcc -static-libstdc++ \
        2>/dev/null || \
    $WIN64 $CFLAGS \
        -o "$OUT/packMP3_win_x64.exe" \
        $SRC \
        -static -static-libgcc -static-libstdc++ \
        && ok "bin/packMP3_win_x64.exe"
fi

# --- Windows x86 (32-bit) ----------------------------------------------------

echo ""
echo "==> Windows x86 (32-bit)"
WIN32="i686-w64-mingw32-g++"
if ! command -v $WIN32 &>/dev/null; then
    echo "    [--] $WIN32 not found, skipping Windows x86"
    echo "         Install with: sudo apt install mingw-w64"
else
    $WIN32 $CFLAGS \
        -o "$OUT/packMP3_win_x86.exe" \
        $SRC icons.res \
        -static -static-libgcc -static-libstdc++ \
        2>/dev/null || \
    $WIN32 $CFLAGS \
        -o "$OUT/packMP3_win_x86.exe" \
        $SRC \
        -static -static-libgcc -static-libstdc++ \
        && ok "bin/packMP3_win_x86.exe"
fi

# --- Summary -----------------------------------------------------------------

echo ""
echo "Output binaries:"
ls -lh "$OUT"/ 2>/dev/null || echo "(none)"

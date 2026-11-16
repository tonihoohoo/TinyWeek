#!/bin/sh
# Build TinyWeek.exe with MinGW-w64.
#   Linux:   sudo apt install gcc-mingw-w64-x86-64 && ./build.sh
#   Windows: MSYS2 "UCRT64" shell: pacman -S mingw-w64-ucrt-x86_64-gcc && CROSS= ./build.sh
# The version comes from the VERSION file (override: VERSION=1.2.3 ./build.sh).
set -e
cd "$(dirname "$0")"
CROSS=${CROSS-x86_64-w64-mingw32-}
VERSION=${VERSION:-$(cat VERSION)}
V4=$(echo "$VERSION" | awk -F. '{printf "%d,%d,%d,0", $1, $2, $3}')
mkdir -p build

${CROSS}windres -I res -DTW_VERSION="$V4" -DTW_VERSION_STR="\\\"$VERSION\\\"" \
  res/tinyweek.rc -O coff -o build/tinyweek.res

# No C runtime: custom entry point, only system DLLs are imported.
${CROSS}gcc -Os -s -DUNICODE -D_UNICODE -Wall -DTW_VERSION_STR="\"$VERSION\"" \
  -ffreestanding -fno-builtin -fno-asynchronous-unwind-tables -fno-ident \
  -fno-stack-check -fno-stack-protector -mno-stack-arg-probe \
  -nostdlib -mwindows -e entry \
  -Wl,--gc-sections -Wl,--file-alignment=512 -Wl,--section-alignment=4096 \
  -Wl,--no-insert-timestamp -Wl,--build-id=none \
  -o build/TinyWeek.exe src/tinyweek.c build/tinyweek.res \
  -lkernel32 -luser32 -lgdi32 -lshell32 -ladvapi32 -lwinhttp -lbcrypt -ldwmapi

ls -l build/TinyWeek.exe

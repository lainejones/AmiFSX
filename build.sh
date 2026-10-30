#!/bin/sh
# Build AmiFSX with amiga-gcc. Run inside WSL:
#   cd /mnt/c/projects/AmiFSX && ./build.sh
set -e
export PATH=/opt/amiga/bin:$PATH

CC=m68k-amigaos-gcc
CFLAGS="-O2 -noixemul -Wall -Wno-pointer-sign -fomit-frame-pointer"

mkdir -p out
# strip at link time with -s, never with m68k-amigaos-strip (corrupts relocs)
$CC $CFLAGS -s -o out/AmiFSX src/amifsx.c
ls -l out/AmiFSX

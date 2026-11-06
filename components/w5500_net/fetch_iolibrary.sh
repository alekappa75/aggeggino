#!/usr/bin/env bash
# Fetches the WIZnet ioLibrary sources the W5500 needs (hardware stack).
# Run once, after cloning the project and before building.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
DST="$HERE/ioLibrary"
REPO="https://github.com/Wiznet/ioLibrary_Driver.git"

if [ -d "$DST/.git" ] || [ -d "$DST/Ethernet" ]; then
    echo "ioLibrary already present in $DST"; exit 0
fi

TMP="$(mktemp -d)"
echo "Cloning ioLibrary into $TMP ..."
git clone --depth 1 "$REPO" "$TMP/iol"

mkdir -p "$DST"
# what we need: wizchip_conf, the W5500 driver, and socket.c (BSD-like)
cp -r "$TMP/iol/Ethernet/wizchip_conf.c" "$DST/"
cp -r "$TMP/iol/Ethernet/wizchip_conf.h" "$DST/"
cp -r "$TMP/iol/Ethernet/socket.c"       "$DST/"
cp -r "$TMP/iol/Ethernet/socket.h"       "$DST/"
mkdir -p "$DST/W5500"
cp -r "$TMP/iol/Ethernet/W5500/w5500.c"  "$DST/W5500/"
cp -r "$TMP/iol/Ethernet/W5500/w5500.h"  "$DST/W5500/"
rm -rf "$TMP"

echo "Done. Files in $DST :"
find "$DST" -type f | sed "s|$DST/|  |"
echo
echo "NOTE: in wizchip_conf.h make sure _WIZCHIP_ is defined as W5500"
echo "      (or pass it as -D_WIZCHIP_=5500 in CMakeLists, already set up)."

#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Pack a built plugin and the overlay for a GitHub Release:
#   ci/package.sh aarch64 0.2.0 OUTDIR    -> OUTDIR/rpi-gpio-spdif-0.2.0-aarch64.tar.gz
set -e
ARCH=$1
VERSION=$2
OUT=$3
NAME=rpi-gpio-spdif-$VERSION-$ARCH
SO=build/$ARCH/libasound_module_pcm_spdifbmc.so
DTBO=build/overlay/spdif-gpio21.dtbo
[ -f "$SO" ] || { echo "$SO missing: run ci/build.sh $ARCH first" >&2; exit 1; }
[ -f "$DTBO" ] || { echo "$DTBO missing: run ci/build.sh overlay first" >&2; exit 1; }
rm -rf "build/$NAME"
mkdir -p "build/$NAME" "$OUT"
cp "$SO" "$DTBO" overlay/spdif-gpio21-overlay.dts README.md LICENSE "build/$NAME/"
tar czf "$OUT/$NAME.tar.gz" --owner=0 --group=0 -C build "$NAME"
rm -rf "build/$NAME"
echo "$OUT/$NAME.tar.gz"

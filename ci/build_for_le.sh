#!/bin/sh
# Everything for one LibreELEC release, from the repository root:
#   python3 ci/le_kernel.py derive 12.2.1 --config build/le/linux.conf > build/le/kernel.env
#   ci/build_for_le.sh build/le/kernel.env build/le/linux.conf REV OUTDIR
# Prepares the kernel tree, builds module and overlay and packs the add-on zip into OUTDIR.
# REV is the last part of the add-on version.
set -e
ENVFILE=$1
CONFIG=$2
REV=$3
OUT=$4
PARENT=build/kernel
. "$ENVFILE"
KTREE=$PARENT/linux-$KCOMMIT
TMP=$OUT/tmp-$LE

sh ci/prepare_kernel.sh "$ENVFILE" "$CONFIG" "$PARENT"
KVER=$(sh ci/build_module.sh "$KTREE" "$TMP" "$KVER_HINT" | tail -n 1)
dtc -@ -Wno-unit_address_vs_reg -I dts -O dtb -o "$TMP/rpi-gpio-spdif.dtbo" driver/rpi-gpio-spdif-overlay.dts
python3 libreelec/build_addon.py --ko "$TMP/rpi_gpio_spdif.ko" --dtbo "$TMP/rpi-gpio-spdif.dtbo" \
    --kver "$KVER" --le "$LE" --rev "$REV" --out "$OUT"
rm -rf "$TMP"

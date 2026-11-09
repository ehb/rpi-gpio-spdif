#!/bin/sh
# Build the module against a prepared kernel tree and check its vermagic.
#   ci/build_module.sh KTREE OUTDIR [KVER_HINT]
# Prints the kernel release on the last line. No Module.symvers exists for a tree that only
# went through modules_prepare, so modpost is told to warn about the imported symbols; the
# kernels this targets are built without MODVERSIONS, so no symbol CRCs are needed.
set -e
KTREE=$1
OUT=$2
HINT=${3:-}
MAKE="make -C $KTREE ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-"
mkdir -p "$OUT"

$MAKE -s M="$PWD/driver" clean
$MAKE M="$PWD/driver" KBUILD_MODPOST_WARN=1 W=1 KCFLAGS=-Werror -j"$(nproc)" modules
KVER=$($MAKE -s kernelrelease)
python3 ci/check_vermagic.py driver/rpi_gpio_spdif.ko "$KTREE/.config" "$KVER" "$HINT" >&2
cp driver/rpi_gpio_spdif.ko "$OUT/"
echo "$KVER"

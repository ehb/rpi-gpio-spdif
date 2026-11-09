#!/bin/sh
# Prepare the kernel tree of one LibreELEC release for out-of-tree modules.
#   ci/prepare_kernel.sh ENVFILE CONFIG TREEPARENT      (ENVFILE from le_kernel.py derive)
# The tree goes to TREEPARENT/linux-<commit>. A finished tree (cache hit) is left as it is;
# the marker holds the commit and config checksum, so a changed config rebuilds.
set -e
. "$1"
CONFIG=$2
PARENT=$3
KTREE=$PARENT/linux-$KCOMMIT
STAMP="$KCOMMIT $KCONFIG_SHA256"
MAKE="make -C $KTREE ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)"

if [ "$(cat "$KTREE/.ci-prepared" 2>/dev/null)" = "$STAMP" ]; then
    echo "kernel tree ready (cached): $KTREE"
    exit 0
fi

rm -rf "$KTREE"
mkdir -p "$PARENT"
echo "downloading $KURL"
curl -fsSL --retry 3 -o "$PARENT/linux.tar.gz" "$KURL"
echo "$KSHA256  $PARENT/linux.tar.gz" | sha256sum -c -
tar xzf "$PARENT/linux.tar.gz" --no-same-owner -C "$PARENT"
rm -f "$PARENT/linux.tar.gz"
[ -d "$KTREE" ] || { echo "archive did not unpack to $KTREE" >&2; exit 1; }

cp "$CONFIG" "$KTREE/.config"
$MAKE -s olddefconfig
"$KTREE/scripts/config" --file "$KTREE/.config" --disable LOCALVERSION_AUTO --set-str LOCALVERSION ""
$MAKE -s olddefconfig
$MAKE modules_prepare
echo "$STAMP" > "$KTREE/.ci-prepared"
echo "kernel tree prepared: $KTREE ($($MAKE -s kernelrelease))"

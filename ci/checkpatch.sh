#!/bin/sh
# Kernel coding style of the driver, with checkpatch from a kernel tree.
#   ci/checkpatch.sh KTREE
# Sources are named one by one: the build leaves a generated .mod.c in driver/.
# PREFER_KERNEL_TYPES is ignored because spdif_bmc.h is also compiled in userspace for
# the encoder test and uses the stdint types on purpose.
set -e
perl "$1/scripts/checkpatch.pl" --no-tree --no-summary --terse --strict --show-types \
    --ignore FILE_PATH_CHANGES,SPDX_LICENSE_TAG,PREFER_KERNEL_TYPES \
    -f driver/rpi_gpio_spdif_main.c driver/spdif_bmc.c driver/spdif_bmc.h

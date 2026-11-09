#!/bin/sh
# Userspace test of the S/PDIF encoder against the golden vectors.
set -e
mkdir -p build
gcc -O2 -Wall -Wextra -Werror -Idriver -o build/bmc_check test/bmc_check.c driver/spdif_bmc.c -lm
build/bmc_check test/golden_raspdif.raw

# SPDX-License-Identifier: GPL-2.0-only
# Out-of-tree build against a prepared kernel tree (modules_prepare):
#   make -C <kernel> M=$PWD modules             (native)
#   make -C <kernel> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M=$PWD modules
obj-m += rpi_gpio_spdif.o
rpi_gpio_spdif-y := rpi_gpio_spdif_main.o spdif_bmc.o

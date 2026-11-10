#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Tests that need a native build: encoder against the golden vectors, the plugin against a
# file slave, the add-on helper. Run ci/build.sh native, aarch64 and overlay first.
set -e
make test BUILD=build/native CC=gcc CFLAGS="-O2 -g -Wall -Wextra -Werror"
TS_SO=build/native/libasound_module_pcm_spdifbmc.so TS_SO_A64=build/aarch64/libasound_module_pcm_spdifbmc.so \
    TS_DTBO=build/overlay/spdif-gpio21.dtbo sh test/test_addon.sh

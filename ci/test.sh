#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Tests that need a native build: encoder against the golden vectors, the plugin against a
# file slave, the add-on helper. Run ci/build.sh native and aarch64 first.
set -e
make test BUILD=build/native CC=gcc CFLAGS="-O2 -g -Wall -Wextra -Werror"
TS_SO=build/native/libasound_module_pcm_spdifbmc.so TS_SO_A64=build/aarch64/libasound_module_pcm_spdifbmc.so \
    sh test/test_addon.sh

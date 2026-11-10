# SPDX-License-Identifier: GPL-2.0-only
# rpi-gpio-spdif: the ALSA plugin spdifbmc and the tests of its encoder
#
# Needs the ALSA headers (alsa/asoundlib.h, alsa/pcm_external.h), not the library, for
# the plugin: it is loaded into a process that already has libasound. Cross build example
# (headers copied from a target-compatible tree):
#   make plugin CC=aarch64-linux-gnu-gcc ALSA_INC=/path/to/dir-containing-alsa
# The device tree overlay (only GPIO21, Pi 0-4) needs dtc: make overlay
ALSA_INC ?= /usr/include
BUILD ?= build
CFLAGS ?= -O2 -g -Wall -Wextra
DTC ?= dtc
SO = libasound_module_pcm_spdifbmc.so
DTBO = spdif-gpio21.dtbo

all: plugin

plugin: $(BUILD)/$(SO)

overlay: $(BUILD)/$(DTBO)

$(BUILD)/$(DTBO): overlay/spdif-gpio21-overlay.dts
	mkdir -p $(BUILD)
	$(DTC) -@ -q -I dts -O dtb -o $@ $<

$(BUILD)/$(SO): src/pcm_spdifbmc.c src/spdif_bmc.c src/spdif_bmc.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fPIC -shared -I$(ALSA_INC) -Isrc -DPIC src/pcm_spdifbmc.c -o $@

$(BUILD)/bmc_check: test/bmc_check.c src/spdif_bmc.c src/spdif_bmc.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc test/bmc_check.c src/spdif_bmc.c -o $@ -lm

# Runs on a machine with alsa-lib and aplay; no audio hardware needed.
test: $(BUILD)/$(SO) $(BUILD)/bmc_check
	$(BUILD)/bmc_check test/golden_raspdif.raw
	python3 test/test_plugin.py $(CURDIR)/$(BUILD)/$(SO)

clean:
	$(RM) -r $(BUILD)

.PHONY: all plugin overlay test clean

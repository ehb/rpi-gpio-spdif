# Changelog

## 0.2.0

The project is now an ALSA plugin; the kernel driver is gone from the main branch. Version
0.1.0 (tag v0.1.0) keeps the kernel driver, for a real sound card and for dmix.

- ALSA plugin `spdifbmc`: encodes S/PDIF (biphase-mark) in software and plays it as S32_LE
  stereo at twice the sample rate into the stock bcm2835-i2s driver, through the overlay
  hifiberry-dac. No kernel module, so it does not depend on the kernel version.
- Kodi service add-on `service.rpi-gpio-spdif` (aarch64): loads the overlay, writes the ALSA
  device `hdmi_bmc` (AC3/DTS passthrough) with the setting `extra_ms`, brings the plugin.
- Tests without hardware: encoder against the golden vectors, plugin against a file slave
  with a receiver-style decoder, add-on helper and packaging.
- Removed: kernel module, card overlay, kernel build in CI, LibreELEC driver add-on.

## 0.1.0

First public version.

- Kernel driver `rpi_gpio_spdif`: S/PDIF on GPIO21 of a Raspberry Pi 0-4 through the
  PCM/I2S block, encoded in software. 32 to 96 kHz, 16/24/32 bit, AC3/DTS passthrough
  through the IEC958 controls, `runtime->delay` for lip sync, Keep Alive.
- Device tree overlay `rpi-gpio-spdif`, alsa-lib card configuration, LibreELEC add-on
  (`driver.audio.rpi-gpio-spdif`, built for LibreELEC 12.2.1).
- Encoder test against raspdif's encoder and a reference implementation; tools for test
  signals, pause and channel status checks.
- Companion project alsa-leadtap (separate) for multi-room audio and visualisation: an
  ALSA plugin between the application and this card; the driver is playback only.

# Changelog

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

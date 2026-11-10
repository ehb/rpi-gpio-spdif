# Changelog

## 0.3.0

- alsa-lib's `iec958` plugin now builds the S/PDIF sub-frames, the channel status and the
  parity; `spdifbmc` only does the biphase-mark coding and takes only `IEC958_SUBFRAME_LE`.
  The device `hdmi_bmc` is an `iec958` device with `spdifbmc` behind it; Kodi's channel
  status bytes become its `status`. The plugin options `aes0..3` and `nonaudio` are gone
  (a hand-made configuration has to put `iec958` in front, see README).
- `spdifbmc` offers mmap access (alsa-lib plugins in front need it) and always takes all
  frames it is given, so `iec958`'s 192-frame blocks cannot slip.
- Lead silence (`extra_ms`) in whole 192-frame blocks.
- Fixed: the plugin offered pause, but the Pi's DMA engine cannot pause, so a player that
  paused got an error. It no longer offers pause (players stop and restart instead; Kodi
  never paused this way).
- Leaner: no own output queue or waiting (ioplug never passes more than fits, and the hw
  device takes it whole), drain and hw_free left to ALSA, the encoder without bit reversal.
  About 310 lines of C, from 570 in 0.2.0.
- Own device tree overlay `spdif-gpio21` instead of borrowing `hifiberry-dac`: a
  `simple-audio-card` with the kernel's generic S/PDIF transmitter codec, the Pi as clock
  producer, and only GPIO21 claimed (GPIO18-20 stay free). The sound card is now
  `spdifgpio21`. Pi 0-4.
- The add-on brings the overlay and applies it; where it does not apply or its card does
  not appear (for example on a Pi 5), it falls back to `hifiberry-dac` as before.
- The release tarballs contain the overlay (`.dtbo` and source).

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

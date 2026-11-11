# Changelog

## 0.3.4

- Documentation only, no change to the plugin, the overlay or the add-on's behaviour.
- The overlay `spdif-gpio21` has been merged into the Raspberry Pi kernel (`rpi-6.18.y`,
  raspberrypi/linux#7748); not yet in a released kernel, so the add-on keeps bringing its
  own copy.
- The overlay alone was run on a Pi 2 and a Pi 5 by a Raspberry Pi engineer (I2S clock and
  data as expected). The whole chain with a receiver is still tested only on a Pi 4B.

## 0.3.3

- Fixed: an add-on update (or a settings change) while Kodi played removed the overlay under
  the open sound card; Kodi lost the device and switched to HDMI. The add-on now keeps the
  overlay applied while the card is in use and only rewrites the ALSA configuration.
- The add-on also recognises an applied overlay from the kernel's configfs; after a boot,
  `dtoverlay -l` could list nothing although the overlay was applied.

## 0.3.2

- One overlay for all models: `spdif-gpio21` now adds its own pin group for GPIO21 (the
  legacy `brcm,pins`/`brcm,function` settings) and points the I2S block at it, instead of
  rewriting `i2s_pins`. The RP1 pin driver of the Pi 5 maps GPIO21 ALT0 to I2S0, so the
  separate `spdif-gpio21-pi5` of 0.3.1 is gone (hint from Raspberry Pi). Checked on a Pi 4B
  (pin 21 owned by the I2S block, 18-20 unclaimed); Pi 5 untested.

## 0.3.1

- Pi 5 variant of the overlay, `spdif-gpio21-pi5` (RP1 I2S0, GPIO21 only); the add-on picks
  it by the device tree's compatible (`brcm,bcm2712`). Untested: checked only by applying it
  to the Pi 5 base device tree.
- Overlay prepared for the Raspberry Pi kernel: the Pi as bit and frame clock producer is now
  stated explicitly (before it came from simple-audio-card's fallback, same result), SPDX
  line, the unused `i2s-controller` property is gone. Checked on a Pi 4B.
- The release tarballs contain both overlays.

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

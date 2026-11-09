# rpi-gpio-spdif

S/PDIF (optical TOSLINK or coaxial) audio output on a Raspberry Pi GPIO, without an
audio HAT and without a transceiver chip. A Linux kernel driver encodes the S/PDIF
signal in software and sends it out through the Pi's PCM/I2S block on GPIO21; a
TOSLINK transmitter module on that pin is all the hardware you need.

- A real ALSA sound card: PCM stereo at 32 / 44.1 / 48 / 88.2 / 96 kHz, 16/24/32 bit (S/PDIF carries 24 significant bits;
  the lowest 8 bits of S32 are dropped).
- **AC3/DTS passthrough** through the standard IEC958 control (non-audio flag): Kodi and
  other players offer passthrough on the `iec958` device as they do for HDMI.
- **Correct latency reporting** (`snd_pcm_delay`), so players keep picture and sound in
  sync.
- **Keep-alive**: between tracks the driver sends encoded silence, so the receiver
  stays locked and the start of the next track is not cut off.

Hardware scope: Raspberry Pi 0-4 (BCM2835 to BCM2711). Not the Pi 5: its I2S block is a
different one. Only the Pi 4 has been tested, and the ready-made LibreELEC add-on zips
exist only for the Pi 4 (see below).

## Status

Tested on a **Raspberry Pi 4B** with LibreELEC 12.2.1 (kernel 6.12.56), Kodi 21 and a
Yamaha receiver on its optical input:

- PCM at 32, 44.1, 48, 88.2 and 96 kHz and in S32: each signal plays for the right time, and
  the bit clock in the kernel's clock tree is rate x 128.
- AC3 and DTS passthrough from Kodi: the receiver shows Dolby Digital for AC3 and plays the
  DTS test signal; both reach the card with the non-audio flag set.
- Lip sync with a flash/click test video in Kodi, checked by eye, sound as PCM through
  alsa-leadtap.
- 20 rate switches between 44.1 and 48 kHz, pause/resume (playing time correct to 10 ms),
  `dmix` with two streams at different rates.
- Open/close stress: 60 short plays at changing rates, 20 players killed in mid-stream,
  Keep Alive on and off: no kernel messages from the driver.
- About 2 hours of internet radio: no late DMA callbacks, no FIFO underruns; softirq load
  about 1 % of one Cortex-A72 core.
- The encoder is byte-identical to raspdif's encoder (golden vectors) and to a plain
  reference implementation over 200,000 random frames (`test/bmc_check.c`).
- The LibreELEC add-on: disable/enable, uninstall and reinstall, update while Kodi plays
  (Kodi reopens the card by itself after about 5 s), refusing a mismatching kernel, by
  calling LibreELEC's add-on hooks the way Kodi calls them. Installing the zip through
  Kodi's user interface (as an update over an installed version) works.

Not tested: other Pi models (only a Pi 4B was available), other receivers, kernels
other than 6.12 (6.18 builds, but was never loaded), Raspberry Pi OS (the build
and ALSA card steps below are written for it, but only LibreELEC has run). Suspend: the Raspberry Pi kernel offers no sleep state
(`/sys/power/state` is empty); the driver stops feeding on a SUSPEND trigger but has
no resume path.

## Hardware

A TOSLINK transmitter module (three pins: data, Vcc, GND) -- for example one taken from
an old PC sound card or bought as a module.

| Module | Raspberry Pi |
|---|---|
| Data | GPIO21 / PCM_DOUT, header pin 40 |
| Vcc | 5 V (pin 2 or 4) -- check your module's data sheet |
| GND | pin 6 (or any GND) |

The 3.3 V level of GPIO21 is enough as "high" for the module that was tested (supplied
with 5 V). If you see dropouts, add a 100 nF capacitor across the module's Vcc and GND.

The driver takes over the I2S block, so it cannot be combined with an I2S DAC/HAT.
GPIO18-20 stay free.

Other ways to get the signal out (not built or tested here):

- **Coaxial, to spec** (0.5 Vpp into 75 ohm): GPIO21 -> 240 ohm -> node; node -> 110 ohm
  -> GND; node -> 100 nF -> centre pin of an RCA socket; socket shield to GND. This gives
  a source impedance of about 75 ohm and 1 V open circuit, 0.5 V into the 75 ohm input.
- **Coaxial, minimal**: GPIO21 -> 100 nF -> RCA centre pin (as suggested by
  rpi-i2s-spdif). That is about 3.3 Vpp, far above the 0.5 Vpp of the standard; many
  inputs accept it, but it is out of spec.
- **Bare red LED** instead of a TOSLINK module (ultra-low-budget): TOSLINK receivers want
  red light around 650 nm. GPIO21 -> 150 ohm -> LED -> GND (about 9 mA with a 2 V LED, below the GPIO
  limit), LED held against the fibre end. A common LED switches fast enough for 44.1/48
  kHz; at 88.2/96 kHz (81 ns per half cell) it may not.

Rates stop at 96 kHz on purpose: 176.4/192 kHz would need a 24.6 MHz PCM clock (not tried),
many TOSLINK parts are specified to 96 kHz, and the main use of 192 kHz on S/PDIF -- E-AC3
(Dolby Digital Plus) passthrough -- is accepted on optical inputs by hardly any receiver.
Kodi transcodes E-AC3 to AC3 instead. Dolby TrueHD and DTS-HD Master Audio need HDMI in any
case.

## LibreELEC add-on

`libreelec/` packages driver, overlay, ALSA configuration and a start unit as a Kodi
add-on (`driver.audio.rpi-gpio-spdif`). A module only loads into the kernel it was built
for, so each zip is for exactly one LibreELEC version. Zips are built only for the
LibreELEC **RPi4 (aarch64) image**, and attached to each release on the
[Releases page](https://github.com/ehb/rpi-gpio-spdif/releases); pick the one that matches
your LibreELEC version exactly. Other Pi models or images would have to be built by hand
(see Build below); that is untested.

The add-on version is `<LibreELEC version>.<revision>`. The release workflow derives the
revision from the tag (`ci/le_kernel.py rev`): major x 10000 + minor x 100 + patch, so the
tag `v0.1.0` gives revision 100 and the zip for LibreELEC 12.2.1 is `12.2.1.100`; a later
`v0.1.1` gives `12.2.1.101`, which Kodi sees as an update. To build a zip yourself
(the revision defaults to 1):

```sh
python libreelec/build_addon.py --ko driver/rpi_gpio_spdif.ko --dtbo rpi-gpio-spdif.dtbo \
    --kver 6.12.56 --le 12.2.1 --rev 100
```

Install the zip in Kodi (Settings -> Add-ons -> Install from zip file; allow unknown
sources first). LibreELEC's add-on hooks then enable the unit, which loads the module and
the overlay before Kodi starts and writes its ALSA part to `/var/lib/alsa/conf.d/50-rpi-gpio-spdif.conf` (a drop-in directory
that LibreELEC's alsa.conf reads; other add-ons can add their own files), so your own
`/storage/.config/asound.conf` stays untouched (and is read after it). Kodi picks up the new
card without a restart. Module options go into `/storage/.config/rpi-gpio-spdif.options`
as one line, e.g. `chunks=12 chunk_frames=256`. After a LibreELEC
update the add-on loads nothing until a matching build is installed, and Kodi shows a
notification saying so.

## Build

You need the kernel tree (or headers with `modules_prepare` done) of the exact kernel
you run.

```sh
make -C /lib/modules/$(uname -r)/build M=$PWD/driver modules
dtc -@ -I dts -O dtb -o rpi-gpio-spdif.dtbo driver/rpi-gpio-spdif-overlay.dts
```

Cross-compiling for a 64-bit Pi: add `ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-`.

## Load

```sh
sudo insmod driver/rpi_gpio_spdif.ko
sudo dtoverlay -d . rpi-gpio-spdif          # at runtime, only while nothing else is bound to I2S
```

or permanently: copy `rpi-gpio-spdif.dtbo` to `/boot/firmware/overlays/` (older systems
`/boot/overlays/`), add `dtoverlay=rpi-gpio-spdif` to `config.txt` and install the module
so that it loads at boot. The card appears as `SPDIFout`:

```
$ cat /proc/asound/cards
 2 [SPDIFout       ]: RPiGPIOSPDIF - RPi S/PDIF out
```

Module parameters (all optional):

| Parameter | Default | Meaning |
|---|---|---|
| `index` | first free | ALSA card index |
| `chunks`, `chunk_frames` | 4, 256 | DMA ring: chunks x frames; the output latency is about `chunks - 1` chunks (4 x 256 at 48 kHz: ~16 ms) |
| `keep_alive` | 1 | initial value of the Keep Alive control |

## ALSA setup

Install `alsa/RPiGPIOSPDIF.conf` as `/usr/share/alsa/cards/RPiGPIOSPDIF.conf`. That gives
the usual `iec958:CARD=SPDIFout` device, which writes the channel status (including the
non-audio flag for passthrough) on open. Where `/usr/share/alsa` is read-only
(LibreELEC), append `alsa/asound-libreelec.conf` to `/storage/.config/asound.conf`
instead. **This is only for a manual install without the LibreELEC add-on:** the add-on
writes `/var/lib/alsa/conf.d/50-rpi-gpio-spdif.conf` itself, and adding the same
definitions to `asound.conf` as well would duplicate them.

```sh
aplay -D iec958:CARD=SPDIFout test.wav
```

**Kodi** lists the card under System -> Audio as "RPi S/PDIF out" and offers it as a
passthrough device (only devices named `iec958`/`spdif`/`hdmi` get passthrough in
Kodi 21). Choose it for both the audio and the passthrough device, enable AC3/DTS.
With speakers set to 2.0, Kodi can also re-encode multichannel sources it cannot pass
through (AAC/Opus 5.1 and similar) to AC3 ("Dolby Digital (AC3) transcoding"). Volume:
Kodi's volume control has no effect on passthrough (the receiver decodes the untouched
bitstream), but it does apply when Kodi transcodes to AC3 (tested with AAC 5.1).

Controls (`amixer -c SPDIFout contents`):

| Control | Meaning |
|---|---|
| IEC958 Playback Default | channel status; sample rate and word length are filled in where left unspecified; a change takes effect at the next 192-frame block |
| IEC958 Playback Con Mask | which status bits can be set |
| Keep Alive Playback Switch | keep sending silence after the last stream closed (default on) |

## Multi-room / visualisation

Use alsa-leadtap (a separate project) between the application and this card. The card
itself is playback only.

## How it works

S/PDIF is biphase-mark coded: every bit becomes two half-cells, and each 32-slot
sub-frame (preamble, 24-bit sample, V, U, C, parity) becomes 64 line bits. Clocking the
PCM block at fs x 128 with 32-bit frames turns one stereo sample frame into exactly four
FIFO words. The driver encodes into a ring of DMA chunks; after each chunk the DMA
callback encodes the chunks that have been played from the ALSA buffer (or silence).
The playing position comes from the DMA residue, `pointer()` reports the frames taken
from the ALSA buffer and `runtime->delay` the encoded frames still ahead of the pin.

Why not a mode of the existing `bcm2835-i2s` driver: that driver hands its data path to
the generic dmaengine PCM, which DMAs straight from the application buffer. Here every
4-byte stereo sample becomes 16 bytes of line code, so the driver needs its own ring and
its own pointer and delay accounting -- the very part a mode would have to replace. The
overlay switches the I2S node to this driver by its compatible string, which also keeps
the two from ever running at the same time.

Receivers can take a second or two to lock after a sample-rate change and may swallow
the start of a stream then. Fixing the player's output rate (Kodi: output configuration
"fixed", 48 kHz) avoids most rate changes.

## Tests and tools

- `test/bmc_check.c` -- encoder against golden vectors and a reference implementation:
  `cc -O2 -Idriver -o bmc_check test/bmc_check.c driver/spdif_bmc.c -lm && ./bmc_check`
- `tools/make_rate_signals.py` -- test signals that tell the rates apart by ear (number
  of beeps, then left and right tones)
- `tools/pause_test.py` -- pause/resume through `snd_pcm_pause`
- `tools/iec958_status.py` -- reads/sets the channel status control (no `iecset` needed)

## Credits

- [raspdif](https://github.com/mill1000/raspdif) by mill1000 (MIT): the software S/PDIF
  encoding scheme this encoder follows, and the source of the nibble table and the golden
  test vectors in `test/` (attributed in `test/bmc_check.c`).
- [rpi-i2s-spdif](https://github.com/kiffie/rpi-i2s-spdif) by kiffie (GPL-2.0): the first
  kernel driver doing S/PDIF through the I2S block.
- The register setup follows the kernel's `sound/soc/bcm/bcm2835-i2s.c`.

## How this was made

The code was written by Claude Code (Anthropic); the commits carry an `Assisted-by:`
trailer. An independent review by a second model found real issues; those are fixed and re-tested
on the device.
I directed the work, made the design decisions and did the testing on my own Raspberry
Pi 4 and receiver, but I have not reviewed the code line by line. Please treat it like
any outside code -- issues and questions are welcome.

## License

GPL-2.0-only (see `LICENSE`).

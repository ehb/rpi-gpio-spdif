# rpi-gpio-spdif

S/PDIF (optical TOSLINK or coaxial) audio output on a Raspberry Pi GPIO, without an
audio HAT and without a transceiver chip. An ALSA plugin encodes the S/PDIF signal in
software and plays it through the Pi's stock I2S driver on GPIO21; a TOSLINK transmitter
module on that pin is all the hardware you need. No kernel module, so nothing depends on
the kernel version.

- **AC3/DTS passthrough**: Kodi offers passthrough on the device `hdmi_bmc` as it does for
  HDMI.
- **PCM** at 32 / 44.1 / 48 / 88.2 / 96 kHz, 16/24/32 bit (S/PDIF carries 24 significant
  bits; the lowest 8 bits of S32 are dropped).
- **Latency is reported** (`snd_pcm_delay`), so players keep picture and sound in sync.
- **Keep-alive in Kodi**: between tracks the card keeps playing silence (see Limits), so
  the receiver stays locked and the start of the next track is not cut off.

Hardware scope: Raspberry Pi 0-4 (BCM2835 to BCM2711), tested only on one Pi 4B. The
Pi 5 is untested: the plugin does not touch the hardware, so it may work, but its I2S block is a
different one; reports are welcome. The ready-made add-on is for 64-bit systems
(aarch64); the plugin tarball is also built for armhf (untested).

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

The plugin uses the I2S block, so it cannot be combined with an I2S DAC/HAT. GPIO18-20
stay free.

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
With its passthrough switched off, Kodi decodes E-AC3 instead. Dolby TrueHD and DTS-HD Master Audio need HDMI in any
case.

## Install

**LibreELEC / Kodi, 64-bit Raspberry Pi.** Install the add-on `service.rpi-gpio-spdif`:

- from the Kodi repository of [pi-lockstep](https://github.com/ehb/pi-lockstep) (Kodi then
  keeps it up to date), or
- from the zip `service.rpi-gpio-spdif-<version>.zip` on the
  [Releases page](https://github.com/ehb/rpi-gpio-spdif/releases) (Settings -> Add-ons ->
  Install from zip file; allow unknown sources first).

**Restart Kodi once after the first install:** a running Kodi does not see the new audio
device. The add-on then applies the stock overlay `hifiberry-dac` when Kodi's services
start, waits for the sound card, and writes the ALSA configuration to
`/var/lib/alsa/conf.d/50-rpi-gpio-spdif.conf` (a drop-in directory that LibreELEC's
alsa.conf reads), so your own `/storage/.config/asound.conf` stays untouched. It depends on
the architecture, not on the LibreELEC version, so a LibreELEC update does not break it. It
refuses to start (and tells you in a notification) if the kernel driver of version 0.1.0 or
raspdif is active.

In Kodi: Settings -> System -> Audio, choose **"S/PDIF on GPIO21" (`hdmi_bmc`)** as audio
output device **and** as passthrough device, and enable AC3/DTS. Kodi offers passthrough only
on devices named `iec958`, `spdif` or `hdmi`, which is why the device name begins with
`hdmi_`. With speakers set to 2.0, Kodi can also re-encode multichannel sources it cannot
pass through (AAC/Opus 5.1 and similar) to AC3 ("Dolby Digital (AC3) transcoding").

**Keep E-AC3, TrueHD and DTS-HD passthrough switched off in Kodi.** Because of its name Kodi takes
the device for HDMI and offers them, but they do not fit through S/PDIF.

**Other systems (Raspberry Pi OS, ...).** Take `rpi-gpio-spdif-<version>-aarch64.tar.gz` (or
`-armhf`) from the Releases page, put the library somewhere persistent, add
`dtoverlay=hifiberry-dac` to `config.txt`, and define the device in `/etc/asound.conf`.
This path has not been run; it is the same configuration the add-on writes:

```
pcm_type.spdifbmc {
    lib "/path/to/libasound_module_pcm_spdifbmc.so"
}

pcm.hdmi_bmc {
    @args [ AES0 AES1 AES2 AES3 ]
    @args.AES0 { type integer default -1 }
    @args.AES1 { type integer default -1 }
    @args.AES2 { type integer default -1 }
    @args.AES3 { type integer default -1 }
    type spdifbmc
    slave.pcm "hw:CARD=sndrpihifiberry"
    extra_ms 80
    aes0 $AES0
    aes1 $AES1
    aes2 $AES2
    aes3 $AES3
}
```

To build the plugin yourself: `make` (needs the ALSA headers; `make test` runs the tests and
also needs `aplay` and Python 3).

### Setting extra_ms

`extra_ms` (add-on setting, default 80, 0 to 500; default 0 when you configure the plugin by hand) keeps that many milliseconds of encoded
silence queued ahead of the audio. The plugin reports the extra delay to the player, which
compensates it, so picture and sound stay in sync. It exists for a player that makes an
early copy of the audio, such as [alsa-leadtap](https://github.com/ehb/alsa-leadtap) in front
of this device: its copy runs ahead of the sound by a fixed lead, and the buffer behind it
has to hold at least that much. With a lead of 180 ms and `extra_ms` 0 the copy was dropped
now and then; 80 fixed it. If nothing makes an early copy, you can set 0. A changed value
applies the next time the device is opened.

## How it works

S/PDIF is biphase-mark coded: every bit becomes two half-cells, and each 32-slot
sub-frame (preamble, 24-bit sample, V, U, C, parity) becomes 64 line bits. Sent as S32_LE
stereo at twice the sample rate, one stereo frame of audio becomes exactly two stereo frames
of 32-bit words: the 64 line bits of one sub-frame are one such frame, bit for bit. The
stock `bcm2835-i2s` driver clocks them out on GPIO21 (PCM_DOUT) at 128 x the sample rate.
The overlay `hifiberry-dac` that ships with the Pi kernel gives that driver a sound card
that accepts S32_LE.

The plugin (`spdifbmc`, an ALSA ioplug) takes ordinary PCM from the application, builds the
sub-frames with the channel status (rate and word length from the stream; the bytes Kodi
passes for passthrough, including the non-audio flag, when it gives them) and writes the
coded words to the card. IEC 61937 data (AC3/DTS) is passed on unchanged as the sample
data. The encoder is `src/spdif_bmc.c`; `test/bmc_check.c` checks it against golden
vectors made by raspdif's encoder and against a plain reference implementation.

## Status

Measured on a **Raspberry Pi 4B** with LibreELEC (kernel 6.12), Kodi 21 and a receiver on its
optical input:

- PCM at 32, 44.1 and 48 kHz and in S32 plays for the right time with a bit clock of
  rate x 128; 88.2 kHz plays; Kodi opens the card at 96 kHz S32.
- 20 rate switches and 40 open/close cycles, 14 players killed in mid-stream, an IEC 61937
  file: no kernel messages; CPU 0.5 to 3 % of one core.
- A Dolby Digital test file played with `aplay` makes the receiver show Dolby Digital. From
  Kodi, PCM, AC3 and DTS were checked for lip sync with a flash/click test video, by eye
  and ear (it fits); the receiver showed Dolby Digital and DTS.
- Keep-alive inside Kodi: after a stop the card keeps playing silence for 30 s (Kodi's
  `audiooutput.streamsilence`), measured.
- Behind alsa-leadtap with `extra_ms` 80: the early copy ran without dropouts (with 0 it did
  not, see above).
- The add-on: installed through Kodi's user interface, and after a reboot of the Pi
  everything came up by itself.
- Selftests without hardware (`make test`, `test/test_addon.sh`): encoder against golden
  vectors, plugin against a file slave decoded like a receiver would, short writes, the
  add-on helper and the zip.

Not tested: other Pi models, Raspberry Pi OS, armhf, other receivers, hours-long runs.

## Limits

- **No `dmix`.** The plugin needs a hw device as its slave and has to mix before encoding.
  For Kodi (exclusive access) that does not matter; two programs at once cannot share the
  output.
- **No keep-alive outside Kodi.** The signal stops when the last stream is
  closed, and the receiver may cut off the start of the next sound. Kodi avoids this by
  keeping the device open with silence; other players do not.
- **The card is named after the overlay** (`sndrpihifiberry`), not "SPDIF".
- **Passthrough relies on the device name** `hdmi_...`: Kodi offers passthrough only for
  names starting with `iec958`, `spdif` or `hdmi`.
- **96 kHz with aplay's default large buffer fails** (hw_params refuse the period); Kodi
  does not hit this.
- Pi 0-4 family by design, but tested only on one Pi 4B. Pi 5 untested.
- The 3.3 V GPIO level and the TOSLINK module are your responsibility; see Hardware.
- Receivers can take a second or two to lock after a sample-rate change and may swallow
  the start of a stream then. Fixing the player's output rate (Kodi: output configuration
  "fixed", 48 kHz) avoids most rate changes.

## Earlier kernel driver

Version 0.1.0 (tag `v0.1.0`) was a kernel driver instead of a plugin: a real ALSA sound card
`SPDIFout` (usable by `dmix` and any program, keep-alive without a player), at the price of
a module that fits exactly one kernel and so one LibreELEC version. It is still there under
that tag, with the zips for LibreELEC 12.2.1, if you need a real card or `dmix`. The plugin
and the driver must not run together.

## Tools

- `tools/make_rate_signals.py` -- test signals that tell the rates apart by ear (number
  of beeps, then left and right tones)

## Credits

- [raspdif](https://github.com/mill1000/raspdif) by mill1000 (MIT): the software S/PDIF
  encoding scheme this encoder follows, and the source of the nibble table and the golden
  test vectors in `test/` (attributed in `test/bmc_check.c`).
- [rpi-i2s-spdif](https://github.com/kiffie/rpi-i2s-spdif) by kiffie (GPL-2.0): the first
  kernel driver doing S/PDIF through the I2S block.
- alsa-lib's `pcm_iec958` plugin: it builds the 32-bit S/PDIF sub-frame (channel status,
  parity) but does no biphase-mark coding, which this plugin adds on top of the same idea.
- Raspberry Pi engineers on their forum, who suggested this approach.

## How this was made

The code was written by Claude Code (Anthropic); the commits carry an `Assisted-by:`
trailer. An independent review by a second model found real issues; those are fixed.
I directed the work, made the design decisions and did the testing on my own Raspberry
Pi 4 and receiver, but I have not reviewed the code line by line. Please treat it like
any outside code -- issues and questions are welcome.

## License

GPL-2.0-only (see `LICENSE`).

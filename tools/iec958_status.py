#!/usr/bin/env python3
"""Read or set the 'IEC958 Playback Default' control of an ALSA card through libasound
(ctypes) -- LibreELEC has no iecset.
  python3 iec958_status.py SPDIFout                 show bytes 0..4
  python3 iec958_status.py SPDIFout nonaudio on|off set/clear AES0 bit 1 (IEC 61937 data)
"""
import ctypes
import sys

asound = ctypes.CDLL("libasound.so.2")
card = sys.argv[1]
ctl = ctypes.c_void_p()
if asound.snd_ctl_open(ctypes.byref(ctl), ("hw:CARD=%s" % card).encode(), 0) < 0:
    sys.exit("cannot open card %s" % card)
val = ctypes.c_void_p()
asound.snd_ctl_elem_value_malloc(ctypes.byref(val))
asound.snd_ctl_elem_value_set_interface(val, 2)  # SND_CTL_ELEM_IFACE_MIXER
asound.snd_ctl_elem_value_set_name(val, b"IEC958 Playback Default")
if asound.snd_ctl_elem_read(ctl, val) < 0:
    sys.exit("cannot read 'IEC958 Playback Default'")
aes = ctypes.create_string_buffer(256)  # snd_aes_iec958_t: status[24], subcode[147], ...
asound.snd_ctl_elem_value_get_iec958(val, aes)
status = bytearray(aes.raw[:24])
if len(sys.argv) >= 4 and sys.argv[2] == "nonaudio":
    if sys.argv[3] == "on":
        status[0] |= 0x02
    else:
        status[0] &= ~0x02 & 0xff
    ctypes.memmove(aes, bytes(status), 24)
    asound.snd_ctl_elem_value_set_iec958(val, aes)
    if asound.snd_ctl_elem_write(ctl, val) < 0:
        sys.exit("cannot write")
    asound.snd_ctl_elem_read(ctl, val)
    asound.snd_ctl_elem_value_get_iec958(val, aes)
    status = bytearray(aes.raw[:24])
print("AES0..4: %s  (non-audio %s)" % (" ".join("%02x" % b for b in status[:5]),
                                          "on" if status[0] & 0x02 else "off"))

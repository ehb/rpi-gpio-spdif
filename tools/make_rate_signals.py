#!/usr/bin/env python3
"""Test signals that tell the rates apart by ear (the receiver does not show the rate):
lead-in silence (receivers are slow to lock after a rate change), N beeps of 440 Hz,
then 1 s 880 Hz left only and 1 s 660 Hz right only. Raw stereo, S16_LE or S32_LE.
  python make_rate_signals.py outdir [lead_s]
Writes lead_32000.raw (1 beep), lead_88200.raw (2), lead_96000.raw (3),
lead_48000_s32.raw (4, S32). Play: aplay -D hw:CARD=SPDIFout -t raw -c 2 -f S16_LE -r 32000 ...
"""
import math
import os
import struct
import sys


def signal(rate, beeps, s32=False, lead=2.0, amp=8000):
    out = [(0, 0)] * int(rate * lead)

    def tone(hz, dur, left=True, right=True):
        for n in range(int(rate * dur)):
            v = int(amp * math.sin(2 * math.pi * hz * n / rate))
            out.append((v if left else 0, v if right else 0))

    def silence(dur):
        out.extend([(0, 0)] * int(rate * dur))

    for _ in range(beeps):
        tone(440, 0.25)
        silence(0.25)
    silence(0.5)
    tone(880, 1.0, right=False)
    silence(0.3)
    tone(660, 1.0, left=False)
    silence(0.3)
    fmt, shift = ('<ii', 16) if s32 else ('<hh', 0)
    return b''.join(struct.pack(fmt, l << shift, r << shift) for l, r in out)


if __name__ == '__main__':
    outdir = sys.argv[1]
    lead = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
    for name, rate, beeps, s32 in (('lead_32000', 32000, 1, False), ('lead_88200', 88200, 2, False),
                                   ('lead_96000', 96000, 3, False), ('lead_48000_s32', 48000, 4, True)):
        open(os.path.join(outdir, name + '.raw'), 'wb').write(signal(rate, beeps, s32, lead))
    print('written to', outdir)

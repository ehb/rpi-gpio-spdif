#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Selftest of the spdifbmc plugin without audio hardware: plays a signal through alsa-lib's
iec958 plugin and spdifbmc into a file slave and decodes what was written like a receiver
would.

    python3 test_plugin.py /path/to/libasound_module_pcm_spdifbmc.so
"""
import math
import os
import struct
import subprocess
import sys
import tempfile

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import bmc_decode as bmc

so = sys.argv[1]

FAILS = 0
FS = {32000: 3, 44100: 0, 48000: 2, 88200: 8, 96000: 10}


def check(name, ok, detail=""):
    global FAILS
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else "  " + str(detail)))
    FAILS += 0 if ok else 1


def byte(bits, i):
    return sum(bits[i * 8 + j] << j for j in range(8))


def run(rate, fmt, status, ems=0, device="bmc"):
    """status: the four channel status bytes for iec958 (a rate of 1 = "not indicated" is
    filled in by iec958). device: what aplay opens (bmc, or bmc:A,B,C,D for @args)."""
    d = tempfile.mkdtemp()
    out = os.path.join(d, "out.raw")
    conf = os.path.join(d, "asound.conf")
    with open(conf, "w", encoding="ascii") as f:
        f.write('</usr/share/alsa/alsa.conf>\npcm_type.spdifbmc { lib "%s" }\n' % so)
        f.write('pcm.sink { type file slave.pcm "null" file "%s" format "raw" }\n' % out)
        f.write('pcm.coder { type spdifbmc slave.pcm "sink" extra_ms %d }\n' % ems)
        f.write('pcm.bmc { @args [ A0 A1 A2 A3 ]\n')
        for i, v in enumerate(status):
            f.write('  @args.A%d { type integer default %d }\n' % (i, v))
        f.write('  type iec958 slave { pcm "coder" format IEC958_SUBFRAME_LE } status [ $A0 $A1 $A2 $A3 ] }\n')
    if device != "bmc":
        status = [int(x) for x in device.split(":")[1].split(",")]
    n = rate // 4
    want = [(int(12000 * math.sin(2 * math.pi * 997 * i / rate)), int(-9000 * math.sin(2 * math.pi * 440 * i / rate)) ) for i in range(n)]
    raw = os.path.join(d, "in.raw")
    with open(raw, "wb") as f:
        if fmt == "S16_LE":
            f.write(b"".join(struct.pack("<hh", l, r) for l, r in want))
        else:
            f.write(b"".join(struct.pack("<ii", l << 16, r << 16) for l, r in want))
    env = dict(os.environ, ALSA_CONFIG_PATH=conf)
    r = subprocess.run(["aplay", "-q", "-D", device, "-t", "raw", "-f", fmt, "-c", "2", "-r", str(rate), raw],
                       env=env, capture_output=True, text=True, timeout=30)
    name = "%d %s status %s%s" % (rate, fmt, ",".join("%d" % v for v in status), " lead %d ms" % ems if ems else "")
    check(name + ": aplay", r.returncode == 0, r.stderr[-300:])
    data = open(out, "rb").read() if os.path.exists(out) else b""
    # aplay pads the last period with silence
    check(name + ": 16 bytes per frame", len(data) >= n * 16 and len(data) % 16 == 0, (len(data), n * 16))
    got, csa, csb, errors = bmc.decode(data)
    check(name + ": decodes without errors", not errors, errors[:3])
    pairs = [(a, b) for a, b, _ in got]
    skip = (rate * ems // 1000 + 191) // 192 * 192  # lead silence in whole blocks
    check(name + ": leading silence", not any(a or b for a, b in pairs[:skip]))
    check(name + ": samples", pairs[skip:skip + n] == want and not any(a or b for a, b in pairs[skip + n:]),
          (pairs[skip:skip + 2], want[:2]))
    starts = [i for i, (_, _, k) in enumerate(got) if k == "B"]
    check(name + ": block start every 192", starts[:3] == [0, 192, 384] and all(b - a == 192 for a, b in zip(starts, starts[1:])),
          starts[:6])
    fs = FS[rate]
    if skip:
        lead = [byte(csa[:192], i) for i in range(4)]
        check(name + ": lead silence status: PCM, rate", lead == [0x04, 0x82, 0x00, fs], lead)
    a, b = csa[skip:skip + 192], csb[skip:skip + 192]
    got_cs = [byte(a, i) for i in range(4)]
    check(name + ": status bytes 0..3 from iec958", got_cs ==
          [status[0], status[1], status[2], (status[3] & ~15) | fs if status[3] & 15 == 1 else status[3]], got_cs)
    check(name + ": same status on both sub-frames", a == b)


PCM = [0x04, 0x82, 0x00, 0x01]  # consumer PCM, rate filled in by iec958
for rate in (32000, 44100, 48000, 88200, 96000):
    run(rate, "S16_LE", PCM)
run(96000, "S32_LE", PCM)
run(48000, "S16_LE", [0x06, 0x82, 0x00, 0x02])            # Kodi's passthrough status
run(48000, "S16_LE", PCM, 0, "bmc:6,130,0,2")         # the same, passed as arguments
run(44100, "S16_LE", PCM, 80)
run(48000, "S16_LE", PCM, 80)
run(96000, "S16_LE", PCM, 80)
print("\nALL OK" if not FAILS else "\n%d FAILED" % FAILS)
sys.exit(1 if FAILS else 0)

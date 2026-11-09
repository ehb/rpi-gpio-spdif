#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Selftest of the spdifbmc plugin without audio hardware: plays a signal into a file slave and
decodes what the plugin wrote like a receiver would.

    python3 test_plugin.py /path/to/libasound_module_pcm_spdifbmc.so

With a C compiler it also builds shortwrite.c, a shim that makes the slave take short writes,
and plays through it.
"""
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import bmc_decode as bmc

so = sys.argv[1]

FAILS = 0


def check(name, ok, detail=""):
    global FAILS
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else "  " + str(detail)))
    FAILS += 0 if ok else 1


def run(rate, fmt, nonaudio, aes="", ems=0, shim=None):
    d = tempfile.mkdtemp()
    out = os.path.join(d, "out.raw")
    conf = os.path.join(d, "asound.conf")
    with open(conf, "w", encoding="ascii") as f:
        f.write('</usr/share/alsa/alsa.conf>\npcm_type.spdifbmc { lib "%s" }\n' % so)
        f.write('pcm.sink { type file slave.pcm "null" file "%s" format "raw" }\n' % out)
        f.write('pcm.bmc { type spdifbmc slave.pcm "sink" nonaudio %s %s }\n' % ("false" if aes else ("true" if nonaudio else "false"), aes + " extra_ms %d" % ems))
    n = rate // 4
    want = [(int(12000 * math.sin(2 * math.pi * 997 * i / rate)), int(-9000 * math.sin(2 * math.pi * 440 * i / rate)) ) for i in range(n)]
    raw = os.path.join(d, "in.raw")
    with open(raw, "wb") as f:
        if fmt == "S16_LE":
            f.write(b"".join(struct.pack("<hh", l, r) for l, r in want))
        else:
            f.write(b"".join(struct.pack("<ii", l << 16, r << 16) for l, r in want))
    env = dict(os.environ, ALSA_CONFIG_PATH=conf)
    if shim:
        env.update(LD_PRELOAD=shim[0], **shim[1])
    r = subprocess.run(["aplay", "-q", "-D", "bmc", "-t", "raw", "-f", fmt, "-c", "2", "-r", str(rate), raw],
                       env=env, capture_output=True, text=True, timeout=30)
    name = "%d %s%s%s" % (rate, fmt, " non-audio" if nonaudio else "", " " + shim[2] if shim else "")
    check(name + ": aplay", r.returncode == 0, r.stderr[-300:])
    data = open(out, "rb").read() if os.path.exists(out) else b""
    # aplay pads the last period with silence
    check(name + ": 16 bytes per frame", len(data) >= n * 16 and len(data) % 16 == 0, (len(data), n * 16))
    got, csa, csb, errors = bmc.decode(data)
    check(name + ": decodes without errors", not errors, errors[:3])
    pairs = [(a, b) for a, b, _ in got]
    skip = rate * ems // 1000
    check(name + ": leading silence", not any(a or b for a, b in pairs[:skip]))
    pairs = pairs[skip:]
    check(name + ": samples", pairs[:n] == want and not any(a or b for a, b in pairs[n:]), (pairs[:2], want[:2]))
    check(name + ": block start every 192", [i for i, (_, _, k) in enumerate(got) if k == "B"][:3] == [0, 192, 384])
    byte = lambda bits, i: sum(bits[i * 8 + j] << j for j in range(8))
    check(name + ": non-audio bit", bool(byte(csa, 0) & 2) == nonaudio, byte(csa, 0))
    fs = {32000: 3, 44100: 0, 48000: 2, 88200: 8, 96000: 10}[rate]
    check(name + ": sample rate in the status", byte(csa, 3) & 15 == fs, byte(csa, 3))
    check(name + ": channel numbers", (byte(csa, 2) >> 4, byte(csb, 2) >> 4) == (1, 2))


for rate in (32000, 44100, 48000, 88200, 96000):
    run(rate, "S16_LE", False)
run(48000, "S32_LE", False)
run(48000, "S16_LE", True)
run(48000, "S16_LE", True, "aes0 6 aes1 130 aes2 0 aes3 2")
run(44100, "S16_LE", False, "", 80)
cc = shutil.which("cc") or shutil.which("gcc")
if cc:
    lib = os.path.join(tempfile.mkdtemp(), "shortwrite.so")
    r = subprocess.run([cc, "-O2", "-fPIC", "-shared", os.path.join(here, "shortwrite.c"), "-o", lib, "-ldl"],
                       capture_output=True, text=True)
    check("build shortwrite shim", r.returncode == 0, r.stderr[-300:])
    run(48000, "S16_LE", False, "", 0, (lib, {}, "short writes"))
    run(44100, "S16_LE", False, "", 80, (lib, {}, "short writes, lead"))
    run(48000, "S16_LE", False, "", 0, (lib, {"SHORT_EAGAIN": "1"}, "short writes, refused"))
else:
    print("skip short-write tests: no C compiler")
print("\nALL OK" if not FAILS else "\n%d FAILED" % FAILS)
sys.exit(1 if FAILS else 0)

#!/usr/bin/env python3
"""On the Raspberry Pi: play a raw S16_LE stereo file on hw:CARD=SPDIFout, pause for a while in
the middle (snd_pcm_pause), resume, and report states and timing. aplay can only pause
interactively. Usage: python3 pause_test.py file.raw rate [pause_s]"""
import ctypes, sys, time

path, rate = sys.argv[1], int(sys.argv[2])
pause_s = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0
a = ctypes.CDLL("libasound.so.2")
a.snd_pcm_state.restype = ctypes.c_int
a.snd_pcm_state_name.restype = ctypes.c_char_p
pcm = ctypes.c_void_p()
if a.snd_pcm_open(ctypes.byref(pcm), b"hw:CARD=SPDIFout", 0, 0) < 0:
    sys.exit("cannot open")
if a.snd_pcm_set_params(pcm, 2, 3, 2, rate, 0, 200000) < 0:  # S16_LE, RW_INTERLEAVED
    sys.exit("cannot set params")
data = open(path, "rb").read()
chunk = 1024 * 4
half = len(data) // 2 // chunk * chunk


def state():
    return a.snd_pcm_state_name(a.snd_pcm_state(pcm)).decode()


def write(buf):
    off = 0
    while off < len(buf):
        n = a.snd_pcm_writei(pcm, buf[off:off + chunk], min(chunk, len(buf) - off) // 4)
        if n < 0:
            print("write error", n)
            a.snd_pcm_recover(pcm, n, 0)
            continue
        off += n * 4


t0 = time.time()
write(data[:half])
print("%.2f s: first half written, state %s" % (time.time() - t0, state()))
rc = a.snd_pcm_pause(pcm, 1)
print("%.2f s: pause -> rc %d, state %s" % (time.time() - t0, rc, state()))
time.sleep(pause_s)
print("%.2f s: still %s" % (time.time() - t0, state()))
rc = a.snd_pcm_pause(pcm, 0)
print("%.2f s: resume -> rc %d, state %s" % (time.time() - t0, rc, state()))
write(data[half:])
a.snd_pcm_drain(pcm)
total = len(data) / 4 / rate
print("%.2f s: done, state %s (audio %.2f s + pause %.1f s = %.2f s expected)"
      % (time.time() - t0, state(), total, pause_s, total + pause_s))
a.snd_pcm_close(pcm)

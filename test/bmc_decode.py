# SPDX-License-Identifier: GPL-2.0-only
"""Receiver-style S/PDIF decoder for the selftest of the spdifbmc plugin."""
import struct


def decode(stream):
    """Bits in transmit order (each 32-bit word MSB first), then: every bit cell starts
    with a transition; preambles are the only violations. Returns (samples, cs_a, cs_b, errors);
    samples are (left, right, preamble of the left sub-frame)."""
    words = struct.unpack('<%dI' % (len(stream) // 4), stream)
    bits = []
    for w in words:
        bits += [(w >> (31 - i)) & 1 for i in range(32)]
    pre = {(1, 1, 1, 0, 1, 0, 0, 0): 'B', (1, 1, 1, 0, 0, 0, 1, 0): 'M', (1, 1, 1, 0, 0, 1, 0, 0): 'W'}
    pre.update({tuple(1 - x for x in k): v for k, v in list(pre.items())})
    out, csa, csb, errors = [], [], [], []
    pos, cur = 0, {}
    while pos + 64 <= len(bits):
        sub = bits[pos:pos + 64]
        kind = pre.get(tuple(sub[:8]))
        if kind is None:
            errors.append('no preamble at bit %d' % pos)
            break
        if pos and sub[0] == bits[pos - 1]:
            errors.append('no transition into preamble at bit %d' % pos)
        val = []
        for i in range(8, 64, 2):
            c0, c1 = sub[i], sub[i + 1]
            if c0 == sub[i - 1]:
                errors.append('missing cell transition at bit %d' % (pos + i))
            val.append(1 if c0 != c1 else 0)
        # val: 28 bits in time order = aux(4) sample(20) V U C P, LSB first per field
        if sum(val) % 2:
            errors.append('parity at bit %d' % pos)
        sample = sum(v << i for i, v in enumerate(val[4:24])) >> 4
        if sample >= 0x8000:
            sample -= 0x10000
        if kind in 'BM':
            cur = {'l': sample, 'kind': kind}
            csa.append(val[26])
        else:
            out.append((cur.get('l'), sample, cur.get('kind')))
            csb.append(val[26])
        pos += 64
    return out, csa, csb, errors

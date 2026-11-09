// SPDX-License-Identifier: GPL-2.0-only
/*
 * S/PDIF biphase-mark encoder, see spdif_bmc.h.
 */
#include "spdif_bmc.h"

#include <string.h>

static u32 bmc_rev32(u32 x)
{
	u32 r = 0;
	int i;

	for (i = 0; i < 32; i++, x >>= 1)
		r = (r << 1) | (x & 1);
	return r;
}

/* Preambles, valid after a sub-frame that ended low (always, thanks to even parity) */
static const u8 bmc_preamble[3] = {
	0xE8,	/* B: sub-frame A, start of block */
	0xE2,	/* M: sub-frame A */
	0xE4,	/* W: sub-frame B */
};

/* Four data bits, MSB first in time, as eight biphase-mark cells starting from low */
static const u8 bmc_nibble[16] = {
	0xCC, 0xCD, 0xCB, 0xCA, 0xD3, 0xD2, 0xD4, 0xD5,
	0xB3, 0xB2, 0xB4, 0xB5, 0xAC, 0xAD, 0xAB, 0xAA,
};

static u64 bmc_subframe(unsigned int pre, s32 sample, unsigned int cbit)
{
	u32 s24 = ((u32)sample >> 8) & 0xffffff;
	u32 data;
	u64 code;
	u8 b;
	int i;

	/*
	 * Bits 0..3 preamble slot, 4..7 aux (sample LSBs), 8..27 sample, 28 V, 29 U,
	 * 30 C, 31 P; reversed so that the first bit in time is the MSB.
	 */
	data = bmc_rev32((s24 & 0xf) << 4 | (s24 >> 4) << 8 | (cbit & 1) << 30);
	b = bmc_nibble[(data >> 24) & 0xf];	/* the preamble ends low */
	code = (u64)bmc_preamble[pre] << 8 | b;
	for (i = 5; i >= 0; i--) {
		b = bmc_nibble[(data >> (4 * i)) & 0xf] ^ (u8)-(b & 1);
		code = code << 8 | b;
	}
	/*
	 * Encoded with P = 0. Even parity means the sub-frame ends low; P is the last
	 * bit, and setting it only flips the last half-cell.
	 */
	return code & ~(u64)1;
}

static void bmc_take_status(struct spdif_bmc *e)
{
	memcpy(e->status_a, e->next_a, SPDIF_BMC_STATUS_BYTES);
	memcpy(e->status_b, e->next_b, SPDIF_BMC_STATUS_BYTES);
	e->next_pending = false;
}

void spdif_bmc_init(struct spdif_bmc *e)
{
	unsigned int p, c;

	memset(e, 0, sizeof(*e));
	for (p = 0; p < 3; p++)
		for (c = 0; c < 2; c++)
			e->silence[p][c] = bmc_subframe(p, 0, c);
}

void spdif_bmc_set_status(struct spdif_bmc *e, const u8 *status)
{
	memcpy(e->next_a, status, SPDIF_BMC_STATUS_BYTES);
	memcpy(e->next_b, status, SPDIF_BMC_STATUS_BYTES);
	if ((status[2] >> 4) == 1)
		e->next_b[2] = (status[2] & 0x0f) | (2 << 4);
	e->next_pending = true;
}

void spdif_bmc_reset(struct spdif_bmc *e)
{
	e->frame = 0;
	if (e->next_pending)
		bmc_take_status(e);
}

void spdif_bmc_encode(struct spdif_bmc *e, u32 *out, s32 left, s32 right)
{
	unsigned int f = e->frame;
	unsigned int ca, cb, pa = f ? 1 : 0;
	u64 a, b;

	if (!f && e->next_pending)
		bmc_take_status(e);
	ca = (e->status_a[f >> 3] >> (f & 7)) & 1;
	cb = (e->status_b[f >> 3] >> (f & 7)) & 1;
	if (!((((u32)left) | ((u32)right)) >> 8)) {	/* nothing in the top 24 bits */
		a = e->silence[pa][ca];
		b = e->silence[2][cb];
	} else {
		a = bmc_subframe(pa, left, ca);
		b = bmc_subframe(2, right, cb);
	}
	out[0] = (u32)(a >> 32);
	out[1] = (u32)a;
	out[2] = (u32)(b >> 32);
	out[3] = (u32)b;
	e->frame = f + 1 == SPDIF_BMC_BLOCK ? 0 : f + 1;
}

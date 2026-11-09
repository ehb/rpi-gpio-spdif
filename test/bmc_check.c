// SPDX-License-Identifier: GPL-2.0-only
/*
 * Userspace checks of the encoder (driver/spdif_bmc.c):
 *   cc -O2 -Idriver -o bmc_check test/bmc_check.c driver/spdif_bmc.c -lm
 *   ./bmc_check test/golden_raspdif.raw
 *
 * 1. Golden vectors: 400 frames encoded by raspdif's own encoder (mill1000/raspdif,
 *    source/spdif.c, run on a Raspberry Pi; test/golden_raspdif.raw, transmit order,
 *    little-endian 32-bit words) must come out byte-identical.
 * 2. A straightforward reference encoder (explicit even parity, no silence table,
 *    status switched only at block starts) against the optimised one, for random
 *    24-bit frames, silence and status changes.
 * Exit code 0 only if both pass.
 *
 * The nibble table below (nib[]) and test/golden_raspdif.raw come from raspdif
 * (https://github.com/mill1000/raspdif, MIT licence); the golden
 * file is the output of raspdif's encoder. The MIT terms allow use under GPL-2.0-only.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spdif_bmc.h"

static const unsigned char nib[16] = { 0xCC, 0xCD, 0xCB, 0xCA, 0xD3, 0xD2, 0xD4, 0xD5,
				       0xB3, 0xB2, 0xB4, 0xB5, 0xAC, 0xAD, 0xAB, 0xAA };

static u32 rev32(u32 x)
{
	u32 r = 0;

	for (int i = 0; i < 32; i++, x >>= 1)
		r = (r << 1) | (x & 1);
	return r;
}

/* raspdif's spdif_build_subframe/spdif_encode_biphase_mark, written out plainly */
static u64 ref_subframe(u8 pre, s32 sample, unsigned int cbit)
{
	u32 s24 = ((u32)sample >> 8) & 0xffffff;
	u32 raw = (s24 & 0xf) << 4 | (s24 >> 4) << 8 | (cbit & 1) << 30;
	u8 b[8];
	u64 c = 0;
	u32 data;

	raw |= (u32)(__builtin_popcount(raw) & 1) << 31;
	data = rev32(raw);
	b[7] = pre;
	b[6] = nib[(data >> 24) & 0xf];
	for (int i = 5; i >= 0; i--)
		b[i] = nib[(data >> (4 * i)) & 0xf] ^ (u8)-(b[i + 1] & 1);
	for (int i = 7; i >= 0; i--)
		c = c << 8 | b[i];
	return c;
}

struct ref {
	u8 a[24], b[24], na[24], nb[24];
	int pending;
	unsigned int f;
};

static void ref_status(struct ref *r, const u8 *cs)
{
	memcpy(r->na, cs, 24);
	memcpy(r->nb, cs, 24);
	if ((cs[2] >> 4) == 1)
		r->nb[2] = (cs[2] & 0x0f) | 0x20;
	r->pending = 1;
}

static void ref_encode(struct ref *r, u32 *out, s32 l, s32 rr)
{
	u64 a, b;

	if (r->f == 0 && r->pending) {
		memcpy(r->a, r->na, 24);
		memcpy(r->b, r->nb, 24);
		r->pending = 0;
	}
	a = ref_subframe(r->f ? 0xE2 : 0xE8, l, (r->a[r->f / 8] >> (r->f % 8)) & 1);
	b = ref_subframe(0xE4, rr, (r->b[r->f / 8] >> (r->f % 8)) & 1);
	out[0] = a >> 32;
	out[1] = (u32)a;
	out[2] = b >> 32;
	out[3] = (u32)b;
	r->f = (r->f + 1) % 192;
}

static int golden(const char *path)
{
	static unsigned char want[400 * 16];
	u8 cs[SPDIF_BMC_STATUS_BYTES] = { 0 };
	struct spdif_bmc e;
	u32 w[SPDIF_BMC_WORDS];
	long bad = 0;
	FILE *f = fopen(path, "rb");

	if (!f || fread(want, 1, sizeof(want), f) != sizeof(want)) {
		fprintf(stderr, "cannot read %s\n", path);
		return 1;
	}
	fclose(f);
	cs[0] = 0x04;	/* consumer, PCM, copy permitted (as raspdif) */
	cs[2] = 0x10;	/* channel 1 (B gets 2) */
	cs[3] = 0x01;	/* sample rate not indicated */
	spdif_bmc_init(&e);
	spdif_bmc_set_status(&e, cs);
	for (int n = 0; n < 400; n++) {
		int l = (int)(12000 * sin(n / 7.0)), r = -(int)(9000 * sin(n / 11.0));

		if (n == 5) {
			l = -32768;
			r = 32767;
		}
		spdif_bmc_encode(&e, w, (s32)((u32)l << 16), (s32)((u32)r << 16));
		/* the file holds little-endian words */
		for (int i = 0; i < SPDIF_BMC_WORDS; i++)
			for (int k = 0; k < 4; k++)
				if (want[n * 16 + i * 4 + k] != ((w[i] >> (8 * k)) & 0xff))
					bad++;
	}
	printf("%s golden vectors (raspdif): %ld of %d bytes differ\n", bad ? "FAIL" : "ok  ", bad,
	       400 * 16);
	return bad != 0;
}

static int random_vs_reference(void)
{
	u8 cs[SPDIF_BMC_STATUS_BYTES];
	u32 w[SPDIF_BMC_WORDS], rw[SPDIF_BMC_WORDS];
	struct spdif_bmc e;
	struct ref ref;
	long bad = 0, n;

	srand(12345);
	spdif_bmc_init(&e);
	memset(&ref, 0, sizeof(ref));
	for (n = 0; n < 200000; n++) {
		s32 l, r;

		if (n % 977 == 0) {	/* new status at an arbitrary point in the block */
			for (int i = 0; i < 24; i++)
				cs[i] = rand() & 0xff;
			spdif_bmc_set_status(&e, cs);
			ref_status(&ref, cs);
		}
		switch (rand() % 4) {
		case 0:
			l = 0;		/* silence table */
			r = 0;
			break;
		case 1:
			l = rand() & 0xff;	/* only below the top 24 bits */
			r = -(rand() & 0xff) & 0xff;
			break;
		default:
			l = (s32)((u32)rand() << 8);
			r = (s32)((u32)rand() << 8 ^ (u32)rand() << 20);
		}
		spdif_bmc_encode(&e, w, l, r);
		ref_encode(&ref, rw, l, r);
		if (memcmp(w, rw, sizeof(w)))
			bad++;
	}
	printf("%s optimised vs reference: %ld of %ld frames differ\n", bad ? "FAIL" : "ok  ", bad, n);
	return bad != 0;
}

int main(int argc, char **argv)
{
	int fails = golden(argc > 1 ? argv[1] : "test/golden_raspdif.raw") + random_vs_reference();

	printf(fails ? "\n%d FAILED\n" : "\nALL OK\n", fails);
	return fails ? 1 : 0;
}

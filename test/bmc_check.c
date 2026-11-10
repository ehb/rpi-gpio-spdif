// SPDX-License-Identifier: GPL-2.0-only
/*
 * Userspace checks of the encoder (src/spdif_bmc.c):
 *   cc -O2 -Isrc -o bmc_check test/bmc_check.c src/spdif_bmc.c -lm
 *   ./bmc_check test/golden_raspdif.raw
 *
 * 1. Golden vectors: 400 frames encoded by raspdif's own encoder (mill1000/raspdif,
 *    source/spdif.c, run on a Raspberry Pi; test/golden_raspdif.raw, transmit order,
 *    little-endian 32-bit words) must come out byte-identical. The sub-frames are built
 *    here the way alsa-lib's iec958 plugin builds them (iec958_subframe), with raspdif's
 *    channel status.
 * 2. A straightforward reference encoder (explicit even parity) against the optimised
 *    one, for random sub-frames: any data, V, U, C, a wrong parity bit, and junk in the
 *    mark bits of the second sub-frame.
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

static uint32_t rev32(uint32_t x)
{
	uint32_t r = 0;

	for (int i = 0; i < 32; i++, x >>= 1)
		r = (r << 1) | (x & 1);
	return r;
}

/* raspdif's spdif_encode_biphase_mark, written out plainly, with explicit even parity */
static uint64_t ref_code(uint8_t pre, uint32_t raw)
{
	uint8_t b[8];
	uint64_t c = 0;
	uint32_t data;

	raw &= 0x7ffffff0;
	raw |= (uint32_t)(__builtin_popcount(raw) & 1) << 31;
	data = rev32(raw);
	b[7] = pre;
	b[6] = nib[(data >> 24) & 0xf];
	for (int i = 5; i >= 0; i--)
		b[i] = nib[(data >> (4 * i)) & 0xf] ^ (uint8_t)-(b[i + 1] & 1);
	for (int i = 7; i >= 0; i--)
		c = c << 8 | b[i];
	return c;
}

static void ref_encode(uint32_t *out, uint32_t a, uint32_t b)
{
	uint64_t ca = ref_code((a & 0xf) == SPDIF_BMC_MARK_B ? 0xE8 : 0xE2, a);
	uint64_t cb = ref_code(0xE4, b);

	out[0] = ca >> 32;
	out[1] = (uint32_t)ca;
	out[2] = cb >> 32;
	out[3] = (uint32_t)cb;
}

/* like alsa-lib's iec958_subframe: mark, top 24 bits of the sample in bits 4..27, C, P */
static uint32_t iec_subframe(uint32_t mark, int32_t sample, unsigned int cbit)
{
	uint32_t data = ((uint32_t)sample >> 4) & ~(uint32_t)0xf;

	data |= (uint32_t)(cbit & 1) << 30;
	data |= (uint32_t)(__builtin_popcount(data) & 1) << 31;
	return data | mark;
}

static int golden(const char *path)
{
	static unsigned char want[400 * 16];
	uint8_t csa[24] = { 0 }, csb[24];
	uint32_t w[SPDIF_BMC_WORDS];
	long bad = 0;
	FILE *f = fopen(path, "rb");

	if (!f || fread(want, 1, sizeof(want), f) != sizeof(want)) {
		fprintf(stderr, "cannot read %s\n", path);
		return 1;
	}
	fclose(f);
	csa[0] = 0x04;	/* consumer, PCM, copy permitted (as raspdif) */
	csa[2] = 0x10;	/* channel 1 */
	csa[3] = 0x01;	/* sample rate not indicated */
	memcpy(csb, csa, sizeof(csb));
	csb[2] = 0x20;	/* channel 2 */
	for (int n = 0; n < 400; n++) {
		int l = (int)(12000 * sin(n / 7.0)), r = -(int)(9000 * sin(n / 11.0));
		unsigned int fr = n % SPDIF_BMC_BLOCK;

		if (n == 5) {
			l = -32768;
			r = 32767;
		}
		spdif_bmc_encode_subframes(w,
			iec_subframe(fr ? SPDIF_BMC_MARK_M : SPDIF_BMC_MARK_B, (int32_t)((uint32_t)l << 16), csa[fr / 8] >> (fr % 8)),
			iec_subframe(SPDIF_BMC_MARK_W, (int32_t)((uint32_t)r << 16), csb[fr / 8] >> (fr % 8)));
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
	uint32_t w[SPDIF_BMC_WORDS], rw[SPDIF_BMC_WORDS];
	long bad = 0, n;

	srand(12345);
	for (n = 0; n < 200000; n++) {
		uint32_t a = (uint32_t)rand() << 16 ^ (uint32_t)rand() << 1 ^ (uint32_t)rand() >> 7;
		uint32_t b = (uint32_t)rand() << 16 ^ (uint32_t)rand() << 2 ^ (uint32_t)rand() >> 9;

		a = (a & ~(uint32_t)0xf) | (rand() % 3 ? SPDIF_BMC_MARK_M : SPDIF_BMC_MARK_B);
		spdif_bmc_encode_subframes(w, a, b);
		ref_encode(rw, a, b);
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

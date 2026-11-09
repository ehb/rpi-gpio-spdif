/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * S/PDIF (IEC 60958) biphase-mark encoder for a plain serial output.
 *
 * Every sub-frame becomes one 64-bit code, sent MSB first: the preamble in the top
 * byte, then 28 biphase-mark coded bits (aux, 20-bit sample, V, U, C, P). On the
 * BCM2835 PCM/I2S block clocked at fs x 128 with 32-bit frames, one stereo sample
 * frame is four 32-bit FIFO words: A.hi, A.lo, B.hi, B.lo.
 *
 * The coding scheme (nibble look-up table, branch-free polarity follow-up, preamble
 * bytes) follows raspdif by mill1000 (MIT); the same idea is in kiffie's
 * rpi-i2s-spdif (GPL-2.0). Even parity makes every sub-frame end on the level it
 * started with, so each 64-bit code can be built on its own.
 */
#ifndef SPDIF_BMC_H
#define SPDIF_BMC_H

#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;

#define SPDIF_BMC_BLOCK		192	/* frames per channel status block */
#define SPDIF_BMC_STATUS_BYTES	(SPDIF_BMC_BLOCK / 8)
#define SPDIF_BMC_WORDS		4	/* 32-bit output words per stereo frame */

struct spdif_bmc {
	u8 status_a[SPDIF_BMC_STATUS_BYTES];	/* channel status being sent */
	u8 status_b[SPDIF_BMC_STATUS_BYTES];
	u8 next_a[SPDIF_BMC_STATUS_BYTES];	/* taken over at the next block start */
	u8 next_b[SPDIF_BMC_STATUS_BYTES];
	bool next_pending;
	unsigned int frame;			/* position in the 192-frame block */
	u64 silence[3][2];			/* codes of a zero sample: [B, M, W][C bit] */
};

/* Set up the silence table; status all zero until spdif_bmc_set_status(). */
void spdif_bmc_init(struct spdif_bmc *e);

/*
 * Channel status (IEC 60958-3 consumer bytes 0..23) for both sub-frames, taken over
 * at the next block start so that no block carries two different states. If it names
 * channel 1 for A (byte 2, bits 4..7), B gets channel 2.
 */
void spdif_bmc_set_status(struct spdif_bmc *e, const u8 *status);

/* Restart at a block start, taking over a pending status at once. */
void spdif_bmc_reset(struct spdif_bmc *e);

/*
 * Encode one stereo frame; samples are left-justified 32-bit, the top 24 bits are
 * sent. Writes SPDIF_BMC_WORDS words in transmit order.
 */
void spdif_bmc_encode(struct spdif_bmc *e, u32 *out, s32 left, s32 right);

#endif

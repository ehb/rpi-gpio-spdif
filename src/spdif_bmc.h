/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * S/PDIF (IEC 60958) biphase-mark encoder for a plain serial output.
 *
 * Input are sub-frames already formatted in the 32-bit layout of ALSA's
 * IEC958_SUBFRAME_LE, as alsa-lib's iec958 plugin writes them: bits 0..3 preamble mark,
 * 4..27 audio, 28 V, 29 U, 30 C, 31 P. Every sub-frame becomes one 64-bit code, sent MSB
 * first: the preamble in the top byte, then 28 biphase-mark coded bits. On the BCM2835
 * PCM/I2S block clocked at fs x 128 with 32-bit slots, one stereo frame is four 32-bit
 * FIFO words: A.hi, A.lo, B.hi, B.lo.
 *
 * The coding scheme (nibble look-up table, branch-free polarity follow-up, preamble
 * bytes) follows raspdif by mill1000 (MIT); the same idea is in kiffie's
 * rpi-i2s-spdif (GPL-2.0). Even parity makes every sub-frame end on the level it
 * started with, so each 64-bit code can be built on its own.
 */
#ifndef SPDIF_BMC_H
#define SPDIF_BMC_H

#include <stdint.h>

#define SPDIF_BMC_BLOCK		192	/* frames per channel status block */
#define SPDIF_BMC_WORDS		4	/* 32-bit output words per stereo frame */

/* alsa-lib's default preamble marks in bits 0..3 (iec958 plugin: Z, X, Y) */
#define SPDIF_BMC_MARK_B	0x08	/* block start, first sub-frame */
#define SPDIF_BMC_MARK_M	0x02	/* first sub-frame */
#define SPDIF_BMC_MARK_W	0x04	/* second sub-frame */

/*
 * Encode one stereo frame given as two sub-frames. The first sub-frame gets preamble B
 * if its mark is SPDIF_BMC_MARK_B, else M; the second always W. The parity bit of the
 * input is ignored: the coding always produces even parity. Writes SPDIF_BMC_WORDS words
 * in transmit order.
 */
void spdif_bmc_encode_subframes(uint32_t *out, uint32_t a, uint32_t b);

#endif

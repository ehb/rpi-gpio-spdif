// SPDX-License-Identifier: GPL-2.0-only
/*
 * S/PDIF biphase-mark encoder, see spdif_bmc.h.
 */
#include "spdif_bmc.h"

/*
 * Four data bits as eight biphase-mark cells starting from low, the first cell in the MSB.
 * Indexed by the bits as they sit in the sub-frame word: bit 0 of the index is sent first.
 */
static const uint8_t bmc_nibble[16] = {
	0xCC, 0xB3, 0xD3, 0xAC, 0xCB, 0xB4, 0xD4, 0xAB,
	0xCD, 0xB2, 0xD2, 0xAD, 0xCA, 0xB5, 0xD5, 0xAA,
};

/*
 * One sub-frame: the preamble byte (valid after a sub-frame that ended low, which even
 * parity guarantees), then bits 4..30 of the IEC 60958 layout, low bit first in time. Each
 * nibble is flipped if the previous cell ended high. Encoded with P = 0; even parity means
 * the sub-frame ends low, and P being the last bit, setting it only flips the last half-cell.
 */
static uint64_t bmc_code(uint8_t preamble, uint32_t bits)
{
	uint64_t code = preamble;
	uint8_t b = 0;
	int i;

	bits &= 0x7ffffff0;
	for (i = 1; i < 8; i++) {
		b = bmc_nibble[(bits >> (4 * i)) & 0xf] ^ (uint8_t)-(b & 1);
		code = code << 8 | b;
	}
	return code & ~(uint64_t)1;
}

void spdif_bmc_encode_subframes(uint32_t *out, uint32_t a, uint32_t b)
{
	uint64_t ca = bmc_code((a & 0xf) == SPDIF_BMC_MARK_B ? 0xE8 : 0xE2, a);	/* B or M */
	uint64_t cb = bmc_code(0xE4, b);						/* W */

	out[0] = (uint32_t)(ca >> 32);
	out[1] = (uint32_t)ca;
	out[2] = (uint32_t)(cb >> 32);
	out[3] = (uint32_t)cb;
}

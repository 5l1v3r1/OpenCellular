/* Copyright 2015 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "dcrypto.h"
#include "internal.h"

#include "registers.h"

static void set_control_register(
	unsigned mode, unsigned key_size, unsigned encrypt)
{
	GWRITE_FIELD(KEYMGR, AES_CTRL, RESET, CTRL_NO_SOFT_RESET);
	GWRITE_FIELD(KEYMGR, AES_CTRL, KEYSIZE, key_size);
	GWRITE_FIELD(KEYMGR, AES_CTRL, CIPHER_MODE, mode);
	GWRITE_FIELD(KEYMGR, AES_CTRL, ENC_MODE, encrypt);
	GWRITE_FIELD(KEYMGR, AES_CTRL, CTR_ENDIAN, CTRL_CTR_BIG_ENDIAN);
	GWRITE_FIELD(KEYMGR, AES_CTRL, ENABLE, CTRL_ENABLE);
}

static int wait_read_data(volatile uint32_t *addr)
{
	int empty;
	int count = 20;     /* Wait these many ~cycles. */

	do {
		empty = REG32(addr);
		count--;
	} while (count && empty);

	return empty ? 0 : 1;
}

int DCRYPTO_aes_init(const uint8_t *key, uint32_t key_len, const uint8_t *iv,
		enum cipher_mode c_mode, enum encrypt_mode e_mode)
{
	int i;
	const uint32_t *p;
	uint32_t key_mode;

	switch (key_len) {
	case 128:
		key_mode = 0;
		break;
	case 192:
		key_mode = 1;
		break;
	case 256:
		key_mode = 2;
		break;
	default:
		/* Invalid key length specified. */
		return 0;
	}
	set_control_register(c_mode, key_mode, e_mode);

	/* Initialize hardware with AES key */
	p = (uint32_t *) key;
	for (i = 0; i < (key_len >> 5); i++)
		GR_KEYMGR_AES_KEY(i) = p[i];
	/* Trigger key expansion. */
	GREG32(KEYMGR, AES_KEY_START) = 1;

	/* Wait for key expansion. */
	if (!wait_read_data(GREG32_ADDR(KEYMGR, AES_KEY_START))) {
		/* Should not happen. */
		return 0;
	}

	/* Initialize IV for modes that require it. */
	if (iv) {
		p = (uint32_t *) iv;
		for (i = 0; i < 4; i++)
			GR_KEYMGR_AES_CTR(i) = p[i];
	}
	return 1;
}

int DCRYPTO_aes_block(const uint8_t *in, uint8_t *out)
{
	int i;
	uint32_t *outw;
	const uint32_t *inw = (const uint32_t *) in;

	/* Write plaintext. */
	for (i = 0; i < 4; i++)
		GREG32(KEYMGR, AES_WFIFO_DATA) = inw[i];

	/* Wait for the result. */
	if (!wait_read_data(GREG32_ADDR(KEYMGR, AES_RFIFO_EMPTY))) {
		/* Should not happen, ciphertext not ready. */
		return 0;
	}

	/* Read ciphertext. */
	outw = (uint32_t *) out;
	for (i = 0; i < 4; i++)
		outw[i] = GREG32(KEYMGR, AES_RFIFO_DATA);
	return 1;
}

void DCRYPTO_aes_write_iv(const uint8_t *iv)
{
	int i;
	const uint32_t *p = (const uint32_t *) iv;

	for (i = 0; i < 4; i++)
		GR_KEYMGR_AES_CTR(i) = p[i];
}

void DCRYPTO_aes_read_iv(uint8_t *iv)
{
	int i;
	uint32_t *p = (uint32_t *) iv;

	for (i = 0; i < 4; i++)
		p[i] = GR_KEYMGR_AES_CTR(i);
}

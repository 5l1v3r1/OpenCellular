/* Copyright 2015 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "dcrypto.h"
#include "internal.h"
#include "registers.h"
#include "util.h"

static const uint8_t *dcrypto_sha256_final(SHA256_CTX *ctx);

#ifdef SECTION_IS_RO
/* RO is single threaded. */
#define mutex_lock(x)
#define mutex_unlock(x)
static inline int dcrypto_grab_sha_hw(void)
{
	return 1;
}
static inline void dcrypto_release_sha_hw(void)
{
}
#else
#include "task.h"
static struct mutex hw_busy_mutex;

static void sha256_init(SHA256_CTX *ctx);
static void sha256_update(SHA256_CTX *ctx, const uint8_t *data, uint32_t len);
static const uint8_t *sha256_final(SHA256_CTX *ctx);
static const uint8_t *sha256_hash(const uint8_t *data, uint32_t len,
				uint8_t *digest);

static const struct HASH_VTAB SW_SHA256_VTAB = {
	sha256_update,
	sha256_final,
	sha256_hash,
	SHA256_DIGEST_BYTES
};

static void sha256_init(SHA256_CTX *ctx)
{
	ctx->vtab = &SW_SHA256_VTAB;
	SHA256_init(&ctx->u.sw_sha256);
}

static void sha256_update(SHA256_CTX *ctx, const uint8_t *data, uint32_t len)
{
	SHA256_update(&ctx->u.sw_sha256, data, len);
}

static const uint8_t *sha256_final(SHA256_CTX *ctx)
{
	return SHA256_final(&ctx->u.sw_sha256);
}

static const uint8_t *sha256_hash(const uint8_t *data, uint32_t len,
				uint8_t *digest)
{
	SHA256_CTX ctx;

	sha256_init(&ctx);
	sha256_update(&ctx, data, len);
	sha256_final(&ctx);

	memcpy(digest, ctx.u.sw_sha256.buf, SHA256_DIGEST_WORDS);

	return digest;
}

static int hw_busy;

int dcrypto_grab_sha_hw(void)
{
	int rv = 0;

	mutex_lock(&hw_busy_mutex);
	if (!hw_busy) {
		rv = 1;
		hw_busy = 1;
	}
	mutex_unlock(&hw_busy_mutex);

	return rv;
}

void dcrypto_release_sha_hw(void)
{
	mutex_lock(&hw_busy_mutex);
	hw_busy = 0;
	mutex_unlock(&hw_busy_mutex);
}

#endif  /* ! SECTION_IS_RO */

void dcrypto_sha_wait(enum sha_mode mode, uint32_t *digest)
{
	int i;
	const int digest_len = (mode == SHA1_MODE) ?
		SHA1_DIGEST_BYTES :
		SHA256_DIGEST_BYTES;

	/* Stop LIVESTREAM mode. */
	GWRITE_FIELD(KEYMGR, SHA_TRIG, TRIG_STOP, 1);
	/* Wait for SHA DONE interrupt. */
	while (!GREG32(KEYMGR, SHA_ITOP))
		;

	/* Read out final digest. */
	for (i = 0; i < digest_len / 4; ++i)
		*digest++ = GR_KEYMGR_SHA_HASH(i);
	dcrypto_release_sha_hw();
}

/* Hardware SHA implementation. */
static const struct HASH_VTAB HW_SHA256_VTAB = {
	dcrypto_sha_update,
	dcrypto_sha256_final,
	DCRYPTO_SHA256_hash,
	SHA256_DIGEST_BYTES
};

void dcrypto_sha_hash(enum sha_mode mode, const uint8_t *data, uint32_t n,
		uint8_t *digest)
{
	dcrypto_sha_init(mode);
	dcrypto_sha_update(NULL, data, n);
	dcrypto_sha_wait(mode, (uint32_t *) digest);
}

void dcrypto_sha_update(struct HASH_CTX *unused,
			const uint8_t *data, uint32_t n)
{
	const uint8_t *bp = (const uint8_t *) data;
	const uint32_t *wp;

	/* Feed unaligned start bytes. */
	while (n != 0 && ((uint32_t)bp & 3)) {
		GREG8(KEYMGR, SHA_INPUT_FIFO) = *bp++;
		n -= 1;
	}

	/* Feed groups of aligned words. */
	wp = (uint32_t *)bp;
	while (n >= 8*4) {
		GREG32(KEYMGR, SHA_INPUT_FIFO) = *wp++;
		GREG32(KEYMGR, SHA_INPUT_FIFO) = *wp++;
		GREG32(KEYMGR, SHA_INPUT_FIFO) = *wp++;
		GREG32(KEYMGR, SHA_INPUT_FIFO) = *wp++;
		GREG32(KEYMGR, SHA_INPUT_FIFO) = *wp++;
		GREG32(KEYMGR, SHA_INPUT_FIFO) = *wp++;
		GREG32(KEYMGR, SHA_INPUT_FIFO) = *wp++;
		GREG32(KEYMGR, SHA_INPUT_FIFO) = *wp++;
		n -= 8*4;
	}
	/* Feed individual aligned words. */
	while (n >= 4) {
		GREG32(KEYMGR, SHA_INPUT_FIFO) = *wp++;
		n -= 4;
	}

	/* Feed remaing bytes. */
	bp = (uint8_t *) wp;
	while (n != 0) {
		GREG8(KEYMGR, SHA_INPUT_FIFO) = *bp++;
		n -= 1;
	}
}

void dcrypto_sha_init(enum sha_mode mode)
{
	/* Stop LIVESTREAM mode, in case final() was not called. */
	GWRITE_FIELD(KEYMGR, SHA_TRIG, TRIG_STOP, 1);
	/* Clear interrupt status. */
	GREG32(KEYMGR, SHA_ITOP) = 0;
	/* SHA1 or SHA256 mode */
	GWRITE_FIELD(KEYMGR, SHA_CFG_EN, SHA1, mode == SHA1_MODE ? 1 : 0);
	/* Enable streaming mode. */
	GWRITE_FIELD(KEYMGR, SHA_CFG_EN, LIVESTREAM, 1);
	/* Enable the SHA DONE interrupt. */
	GWRITE_FIELD(KEYMGR, SHA_CFG_EN, INT_EN_DONE, 1);
	/* Start SHA engine. */
	GWRITE_FIELD(KEYMGR, SHA_TRIG, TRIG_GO, 1);
}

void DCRYPTO_SHA256_init(SHA256_CTX *ctx, uint32_t sw_required)
{
	if (!sw_required && dcrypto_grab_sha_hw()) {
		ctx->vtab = &HW_SHA256_VTAB;
		dcrypto_sha_init(SHA256_MODE);
	}
#ifndef SECTION_IS_RO
	else
		sha256_init(ctx);
#endif
}

static const uint8_t *dcrypto_sha256_final(SHA256_CTX *ctx)
{
	dcrypto_sha_wait(SHA256_MODE, (uint32_t *) ctx->u.buf);
	return ctx->u.buf;
}

const uint8_t *DCRYPTO_SHA256_hash(const uint8_t *data, uint32_t n,
				uint8_t *digest)
{
	if (dcrypto_grab_sha_hw())
		/* dcrypto_sha_wait() will release the hw. */
		dcrypto_sha_hash(SHA256_MODE, data, n, digest);
#ifndef SECTION_IS_RO
	else
		sha256_hash(data, n, digest);
#endif
	return digest;
}

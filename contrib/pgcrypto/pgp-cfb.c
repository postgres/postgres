/*
 * pgp-cfb.c
 *	  Implements both normal and PGP-specific CFB mode.
 *
 * Copyright (c) 2005 Marko Kreen
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in the
 *	  documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.	IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * contrib/pgcrypto/pgp-cfb.c
 */

#include "postgres.h"

#include "mbuf.h"
#include "px.h"
#include "pgp.h"

typedef int (*mix_data_t) (PGP_CFB *ctx, const uint8 *data, int len, uint8 *dst);

struct PGP_CFB
{
	PX_Cipher  *ciph;
	int			block_size;
	int			pos;
	int			block_no;
	int			resync;
	uint8		fr[PGP_MAX_BLOCK];
	uint8		fre[PGP_MAX_BLOCK];
	uint8		encbuf[PGP_MAX_BLOCK];
};

int
pgp_cfb_create(PGP_CFB **ctx_p, int algo, const uint8 *key, int key_len,
			   int resync, uint8 *iv)
{
	int			res;
	PX_Cipher  *ciph;
	PGP_CFB    *ctx;

	res = pgp_load_cipher(algo, &ciph);
	if (res < 0)
		return res;

	res = px_cipher_init(ciph, key, key_len, NULL);
	if (res < 0)
	{
		px_cipher_free(ciph);
		return res;
	}

	ctx = px_alloc(sizeof(*ctx));
	memset(ctx, 0, sizeof(*ctx));
	ctx->ciph = ciph;
	ctx->block_size = px_cipher_block_size(ciph);
	ctx->resync = resync;

	if (iv)
		memcpy(ctx->fr, iv, ctx->block_size);

	*ctx_p = ctx;
	return 0;
}

void
pgp_cfb_free(PGP_CFB *ctx)
{
	px_cipher_free(ctx->ciph);
	memset(ctx, 0, sizeof(*ctx));
	px_free(ctx);
}

/*
 * Data processing for normal CFB.	(PGP_PKT_SYMENCRYPTED_DATA_MDC)
 */
static int
mix_encrypt_normal(PGP_CFB *ctx, const uint8 *data, int len, uint8 *dst)
{
	int			i;

	for (i = ctx->pos; i < ctx->pos + len; i++)
		*dst++ = ctx->encbuf[i] = ctx->fre[i] ^ (*data++);
	ctx->pos += len;
	return len;
}

static int
mix_decrypt_normal(PGP_CFB *ctx, const uint8 *data, int len, uint8 *dst)
{
	int			i;

	for (i = ctx->pos; i < ctx->pos + len; i++)
	{
		ctx->encbuf[i] = *data++;
		*dst++ = ctx->fre[i] ^ ctx->encbuf[i];
	}
	ctx->pos += len;
	return len;
}

/*
 * Data processing for old PGP CFB mode. (PGP_PKT_SYMENCRYPTED_DATA)
 *
 * The goal is to hide the horror from the rest of the code,
 * thus its all concentrated here.
 */
static int
mix_encrypt_resync(PGP_CFB *ctx, const uint8 *data, int len, uint8 *dst)
{
	int			i,
				n;

	/* block #2 is 2 bytes long */
	if (ctx->block_no == 2)
	{
		n = 2 - ctx->pos;
		if (len < n)
			n = len;
		for (i = ctx->pos; i < ctx->pos + n; i++)
			*dst++ = ctx->encbuf[i] = ctx->fre[i] ^ (*data++);

		ctx->pos += n;
		len -= n;

		if (ctx->pos == 2)
		{
			memcpy(ctx->fr, ctx->encbuf + 2, ctx->block_size - 2);
			memcpy(ctx->fr + ctx->block_size - 2, ctx->encbuf, 2);
			ctx->pos = 0;
			return n;
		}
	}
	for (i = ctx->pos; i < ctx->pos + len; i++)
		*dst++ = ctx->encbuf[i] = ctx->fre[i] ^ (*data++);
	ctx->pos += len;
	return len;
}

static int
mix_decrypt_resync(PGP_CFB *ctx, const uint8 *data, int len, uint8 *dst)
{
	int			i,
				n;

	/* block #2 is 2 bytes long */
	if (ctx->block_no == 2)
	{
		n = 2 - ctx->pos;
		if (len < n)
			n = len;
		for (i = ctx->pos; i < ctx->pos + n; i++)
		{
			ctx->encbuf[i] = *data++;
			*dst++ = ctx->fre[i] ^ ctx->encbuf[i];
		}
		ctx->pos += n;
		len -= n;

		if (ctx->pos == 2)
		{
			memcpy(ctx->fr, ctx->encbuf + 2, ctx->block_size - 2);
			memcpy(ctx->fr + ctx->block_size - 2, ctx->encbuf, 2);
			ctx->pos = 0;
			return n;
		}
	}
	for (i = ctx->pos; i < ctx->pos + len; i++)
	{
		ctx->encbuf[i] = *data++;
		*dst++ = ctx->fre[i] ^ ctx->encbuf[i];
	}
	ctx->pos += len;
	return len;
}

/*
 * common code for both encrypt and decrypt.
 */
static int
cfb_process(PGP_CFB *ctx, const uint8 *data, int len, uint8 *dst,
			mix_data_t mix_data)
{
	int			n;
	int			res;

	while (len > 0 && ctx->pos > 0)
	{
		n = ctx->block_size - ctx->pos;
		if (len < n)
			n = len;

		n = mix_data(ctx, data, n, dst);
		data += n;
		dst += n;
		len -= n;

		if (ctx->pos == ctx->block_size)
		{
			memcpy(ctx->fr, ctx->encbuf, ctx->block_size);
			ctx->pos = 0;
		}
	}

	while (len > 0)
	{
		px_cipher_encrypt(ctx->ciph, ctx->fr, ctx->block_size, ctx->fre);
		if (ctx->block_no < 5)
			ctx->block_no++;

		n = ctx->block_size;
		if (len < n)
			n = len;

		res = mix_data(ctx, data, n, dst);
		data += res;
		dst += res;
		len -= res;

		if (ctx->pos == ctx->block_size)
		{
			memcpy(ctx->fr, ctx->encbuf, ctx->block_size);
			ctx->pos = 0;
		}
	}
	return 0;
}

/*
 * public interface
 */

int
pgp_cfb_encrypt(PGP_CFB *ctx, const uint8 *data, int len, uint8 *dst)
{
	mix_data_t	mix = ctx->resync ? mix_encrypt_resync : mix_encrypt_normal;

	return cfb_process(ctx, data, len, dst, mix);
}

int
pgp_cfb_decrypt(PGP_CFB *ctx, const uint8 *data, int len, uint8 *dst)
{
	mix_data_t	mix = ctx->resync ? mix_decrypt_resync : mix_decrypt_normal;

	return cfb_process(ctx, data, len, dst, mix);
}

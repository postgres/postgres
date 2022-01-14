/*
 * internal.c
 *		Wrapper for builtin functions
 *
 * Copyright (c) 2001 Marko Kreen
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
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * contrib/pgcrypto/internal-sha2.c
 */

#include "postgres.h"

#include <time.h>

#include "common/cryptohash.h"
#include "common/sha2.h"
#include "px.h"

void		init_sha224(PX_MD *h);
void		init_sha256(PX_MD *h);
void		init_sha384(PX_MD *h);
void		init_sha512(PX_MD *h);

/* SHA224 */
static unsigned
int_sha224_len(PX_MD *h)
{
	return PG_SHA224_DIGEST_LENGTH;
}

static unsigned
int_sha224_block_len(PX_MD *h)
{
	return PG_SHA224_BLOCK_LENGTH;
}

/* SHA256 */
static unsigned
int_sha256_len(PX_MD *h)
{
	return PG_SHA256_DIGEST_LENGTH;
}

static unsigned
int_sha256_block_len(PX_MD *h)
{
	return PG_SHA256_BLOCK_LENGTH;
}

/* SHA384 */
static unsigned
int_sha384_len(PX_MD *h)
{
	return PG_SHA384_DIGEST_LENGTH;
}

static unsigned
int_sha384_block_len(PX_MD *h)
{
	return PG_SHA384_BLOCK_LENGTH;
}

/* SHA512 */
static unsigned
int_sha512_len(PX_MD *h)
{
	return PG_SHA512_DIGEST_LENGTH;
}

static unsigned
int_sha512_block_len(PX_MD *h)
{
	return PG_SHA512_BLOCK_LENGTH;
}

/* Generic interface for all SHA2 methods */
static void
int_sha2_update(PX_MD *h, const uint8 *data, unsigned dlen)
{
	pg_cryptohash_ctx *ctx = (pg_cryptohash_ctx *) h->p.ptr;

	if (pg_cryptohash_update(ctx, data, dlen) < 0)
		elog(ERROR, "could not update %s context", "SHA2");
}

static void
int_sha2_reset(PX_MD *h)
{
	pg_cryptohash_ctx *ctx = (pg_cryptohash_ctx *) h->p.ptr;

	if (pg_cryptohash_init(ctx) < 0)
		elog(ERROR, "could not initialize %s context", "SHA2");
}

static void
int_sha2_finish(PX_MD *h, uint8 *dst)
{
	pg_cryptohash_ctx *ctx = (pg_cryptohash_ctx *) h->p.ptr;

	if (pg_cryptohash_final(ctx, dst, h->result_size(h)) < 0)
		elog(ERROR, "could not finalize %s context", "SHA2");
}

static void
int_sha2_free(PX_MD *h)
{
	pg_cryptohash_ctx *ctx = (pg_cryptohash_ctx *) h->p.ptr;

	pg_cryptohash_free(ctx);
	pfree(h);
}

/* init functions */

void
init_sha224(PX_MD *md)
{
	pg_cryptohash_ctx *ctx;

	ctx = pg_cryptohash_create(PG_SHA224);
	md->p.ptr = ctx;

	md->result_size = int_sha224_len;
	md->block_size = int_sha224_block_len;
	md->reset = int_sha2_reset;
	md->update = int_sha2_update;
	md->finish = int_sha2_finish;
	md->free = int_sha2_free;

	md->reset(md);
}

void
init_sha256(PX_MD *md)
{
	pg_cryptohash_ctx *ctx;

	ctx = pg_cryptohash_create(PG_SHA256);
	md->p.ptr = ctx;

	md->result_size = int_sha256_len;
	md->block_size = int_sha256_block_len;
	md->reset = int_sha2_reset;
	md->update = int_sha2_update;
	md->finish = int_sha2_finish;
	md->free = int_sha2_free;

	md->reset(md);
}

void
init_sha384(PX_MD *md)
{
	pg_cryptohash_ctx *ctx;

	ctx = pg_cryptohash_create(PG_SHA384);
	md->p.ptr = ctx;

	md->result_size = int_sha384_len;
	md->block_size = int_sha384_block_len;
	md->reset = int_sha2_reset;
	md->update = int_sha2_update;
	md->finish = int_sha2_finish;
	md->free = int_sha2_free;

	md->reset(md);
}

void
init_sha512(PX_MD *md)
{
	pg_cryptohash_ctx *ctx;

	ctx = pg_cryptohash_create(PG_SHA512);
	md->p.ptr = ctx;

	md->result_size = int_sha512_len;
	md->block_size = int_sha512_block_len;
	md->reset = int_sha2_reset;
	md->update = int_sha2_update;
	md->finish = int_sha2_finish;
	md->free = int_sha2_free;

	md->reset(md);
}

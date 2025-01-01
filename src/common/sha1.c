/*-------------------------------------------------------------------------
 *
 *	sha1.c
 *	  Implements the SHA1 Secure Hash Algorithm
 *
 * Fallback implementation of SHA1, as specified in RFC 3174.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/common/sha1.c
 *
 *-------------------------------------------------------------------------
 */

/*	   $KAME: sha1.c,v 1.3 2000/02/22 14:01:18 itojun Exp $    */

/*
 * Copyright (C) 1995, 1996, 1997, and 1998 WIDE Project.
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
 * 3. Neither the name of the project nor the names of its contributors
 *	  may be used to endorse or promote products derived from this software
 *	  without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/*
 * FIPS pub 180-1: Secure Hash Algorithm (SHA-1)
 * based on: http://www.itl.nist.gov/fipspubs/fip180-1.htm
 * implemented by Jun-ichiro itojun Itoh <itojun@itojun.org>
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <sys/param.h>

#include "sha1_int.h"

/* constant table */
static const uint32 _K[] = {0x5a827999, 0x6ed9eba1, 0x8f1bbcdc, 0xca62c1d6};

#define K(t)	_K[(t) / 20]

#define F0(b, c, d) (((b) & (c)) | ((~(b)) & (d)))
#define F1(b, c, d) (((b) ^ (c)) ^ (d))
#define F2(b, c, d) (((b) & (c)) | ((b) & (d)) | ((c) & (d)))
#define F3(b, c, d) (((b) ^ (c)) ^ (d))

#define S(n, x)		(((x) << (n)) | ((x) >> (32 - (n))))

#define H(n)	(ctx->h.b32[(n)])
#define COUNT	(ctx->count)
#define BCOUNT	(ctx->c.b64[0] / 8)
#define W(n)	(ctx->m.b32[(n)])

#define PUTPAD(x) \
do { \
	ctx->m.b8[(COUNT % 64)] = (x);		\
	COUNT++;				\
	COUNT %= 64;				\
	if (COUNT % 64 == 0)			\
		sha1_step(ctx);		\
} while (0)

static void
sha1_step(pg_sha1_ctx *ctx)
{
	uint32		a,
				b,
				c,
				d,
				e;
	size_t		t,
				s;
	uint32		tmp;

#ifndef WORDS_BIGENDIAN
	pg_sha1_ctx tctx;

	memmove(&tctx.m.b8[0], &ctx->m.b8[0], 64);
	ctx->m.b8[0] = tctx.m.b8[3];
	ctx->m.b8[1] = tctx.m.b8[2];
	ctx->m.b8[2] = tctx.m.b8[1];
	ctx->m.b8[3] = tctx.m.b8[0];
	ctx->m.b8[4] = tctx.m.b8[7];
	ctx->m.b8[5] = tctx.m.b8[6];
	ctx->m.b8[6] = tctx.m.b8[5];
	ctx->m.b8[7] = tctx.m.b8[4];
	ctx->m.b8[8] = tctx.m.b8[11];
	ctx->m.b8[9] = tctx.m.b8[10];
	ctx->m.b8[10] = tctx.m.b8[9];
	ctx->m.b8[11] = tctx.m.b8[8];
	ctx->m.b8[12] = tctx.m.b8[15];
	ctx->m.b8[13] = tctx.m.b8[14];
	ctx->m.b8[14] = tctx.m.b8[13];
	ctx->m.b8[15] = tctx.m.b8[12];
	ctx->m.b8[16] = tctx.m.b8[19];
	ctx->m.b8[17] = tctx.m.b8[18];
	ctx->m.b8[18] = tctx.m.b8[17];
	ctx->m.b8[19] = tctx.m.b8[16];
	ctx->m.b8[20] = tctx.m.b8[23];
	ctx->m.b8[21] = tctx.m.b8[22];
	ctx->m.b8[22] = tctx.m.b8[21];
	ctx->m.b8[23] = tctx.m.b8[20];
	ctx->m.b8[24] = tctx.m.b8[27];
	ctx->m.b8[25] = tctx.m.b8[26];
	ctx->m.b8[26] = tctx.m.b8[25];
	ctx->m.b8[27] = tctx.m.b8[24];
	ctx->m.b8[28] = tctx.m.b8[31];
	ctx->m.b8[29] = tctx.m.b8[30];
	ctx->m.b8[30] = tctx.m.b8[29];
	ctx->m.b8[31] = tctx.m.b8[28];
	ctx->m.b8[32] = tctx.m.b8[35];
	ctx->m.b8[33] = tctx.m.b8[34];
	ctx->m.b8[34] = tctx.m.b8[33];
	ctx->m.b8[35] = tctx.m.b8[32];
	ctx->m.b8[36] = tctx.m.b8[39];
	ctx->m.b8[37] = tctx.m.b8[38];
	ctx->m.b8[38] = tctx.m.b8[37];
	ctx->m.b8[39] = tctx.m.b8[36];
	ctx->m.b8[40] = tctx.m.b8[43];
	ctx->m.b8[41] = tctx.m.b8[42];
	ctx->m.b8[42] = tctx.m.b8[41];
	ctx->m.b8[43] = tctx.m.b8[40];
	ctx->m.b8[44] = tctx.m.b8[47];
	ctx->m.b8[45] = tctx.m.b8[46];
	ctx->m.b8[46] = tctx.m.b8[45];
	ctx->m.b8[47] = tctx.m.b8[44];
	ctx->m.b8[48] = tctx.m.b8[51];
	ctx->m.b8[49] = tctx.m.b8[50];
	ctx->m.b8[50] = tctx.m.b8[49];
	ctx->m.b8[51] = tctx.m.b8[48];
	ctx->m.b8[52] = tctx.m.b8[55];
	ctx->m.b8[53] = tctx.m.b8[54];
	ctx->m.b8[54] = tctx.m.b8[53];
	ctx->m.b8[55] = tctx.m.b8[52];
	ctx->m.b8[56] = tctx.m.b8[59];
	ctx->m.b8[57] = tctx.m.b8[58];
	ctx->m.b8[58] = tctx.m.b8[57];
	ctx->m.b8[59] = tctx.m.b8[56];
	ctx->m.b8[60] = tctx.m.b8[63];
	ctx->m.b8[61] = tctx.m.b8[62];
	ctx->m.b8[62] = tctx.m.b8[61];
	ctx->m.b8[63] = tctx.m.b8[60];
#endif

	a = H(0);
	b = H(1);
	c = H(2);
	d = H(3);
	e = H(4);

	for (t = 0; t < 20; t++)
	{
		s = t & 0x0f;
		if (t >= 16)
			W(s) = S(1, W((s + 13) & 0x0f) ^ W((s + 8) & 0x0f) ^ W((s + 2) & 0x0f) ^ W(s));
		tmp = S(5, a) + F0(b, c, d) + e + W(s) + K(t);
		e = d;
		d = c;
		c = S(30, b);
		b = a;
		a = tmp;
	}
	for (t = 20; t < 40; t++)
	{
		s = t & 0x0f;
		W(s) = S(1, W((s + 13) & 0x0f) ^ W((s + 8) & 0x0f) ^ W((s + 2) & 0x0f) ^ W(s));
		tmp = S(5, a) + F1(b, c, d) + e + W(s) + K(t);
		e = d;
		d = c;
		c = S(30, b);
		b = a;
		a = tmp;
	}
	for (t = 40; t < 60; t++)
	{
		s = t & 0x0f;
		W(s) = S(1, W((s + 13) & 0x0f) ^ W((s + 8) & 0x0f) ^ W((s + 2) & 0x0f) ^ W(s));
		tmp = S(5, a) + F2(b, c, d) + e + W(s) + K(t);
		e = d;
		d = c;
		c = S(30, b);
		b = a;
		a = tmp;
	}
	for (t = 60; t < 80; t++)
	{
		s = t & 0x0f;
		W(s) = S(1, W((s + 13) & 0x0f) ^ W((s + 8) & 0x0f) ^ W((s + 2) & 0x0f) ^ W(s));
		tmp = S(5, a) + F3(b, c, d) + e + W(s) + K(t);
		e = d;
		d = c;
		c = S(30, b);
		b = a;
		a = tmp;
	}

	H(0) = H(0) + a;
	H(1) = H(1) + b;
	H(2) = H(2) + c;
	H(3) = H(3) + d;
	H(4) = H(4) + e;

	memset(&ctx->m.b8[0], 0, 64);
}

static void
sha1_pad(pg_sha1_ctx *ctx)
{
	size_t		padlen;			/* pad length in bytes */
	size_t		padstart;

	PUTPAD(0x80);

	padstart = COUNT % 64;
	padlen = 64 - padstart;
	if (padlen < 8)
	{
		memset(&ctx->m.b8[padstart], 0, padlen);
		COUNT += padlen;
		COUNT %= 64;
		sha1_step(ctx);
		padstart = COUNT % 64;	/* should be 0 */
		padlen = 64 - padstart; /* should be 64 */
	}
	memset(&ctx->m.b8[padstart], 0, padlen - 8);
	COUNT += (padlen - 8);
	COUNT %= 64;
#ifdef WORDS_BIGENDIAN
	PUTPAD(ctx->c.b8[0]);
	PUTPAD(ctx->c.b8[1]);
	PUTPAD(ctx->c.b8[2]);
	PUTPAD(ctx->c.b8[3]);
	PUTPAD(ctx->c.b8[4]);
	PUTPAD(ctx->c.b8[5]);
	PUTPAD(ctx->c.b8[6]);
	PUTPAD(ctx->c.b8[7]);
#else
	PUTPAD(ctx->c.b8[7]);
	PUTPAD(ctx->c.b8[6]);
	PUTPAD(ctx->c.b8[5]);
	PUTPAD(ctx->c.b8[4]);
	PUTPAD(ctx->c.b8[3]);
	PUTPAD(ctx->c.b8[2]);
	PUTPAD(ctx->c.b8[1]);
	PUTPAD(ctx->c.b8[0]);
#endif
}

static void
sha1_result(uint8 *digest0, pg_sha1_ctx *ctx)
{
	uint8	   *digest;

	digest = (uint8 *) digest0;

#ifdef WORDS_BIGENDIAN
	memmove(digest, &ctx->h.b8[0], 20);
#else
	digest[0] = ctx->h.b8[3];
	digest[1] = ctx->h.b8[2];
	digest[2] = ctx->h.b8[1];
	digest[3] = ctx->h.b8[0];
	digest[4] = ctx->h.b8[7];
	digest[5] = ctx->h.b8[6];
	digest[6] = ctx->h.b8[5];
	digest[7] = ctx->h.b8[4];
	digest[8] = ctx->h.b8[11];
	digest[9] = ctx->h.b8[10];
	digest[10] = ctx->h.b8[9];
	digest[11] = ctx->h.b8[8];
	digest[12] = ctx->h.b8[15];
	digest[13] = ctx->h.b8[14];
	digest[14] = ctx->h.b8[13];
	digest[15] = ctx->h.b8[12];
	digest[16] = ctx->h.b8[19];
	digest[17] = ctx->h.b8[18];
	digest[18] = ctx->h.b8[17];
	digest[19] = ctx->h.b8[16];
#endif
}

/* External routines for this SHA1 implementation */

/*
 * pg_sha1_init
 *
 * Initialize a SHA1 context.
 */
void
pg_sha1_init(pg_sha1_ctx *ctx)
{
	memset(ctx, 0, sizeof(pg_sha1_ctx));
	H(0) = 0x67452301;
	H(1) = 0xefcdab89;
	H(2) = 0x98badcfe;
	H(3) = 0x10325476;
	H(4) = 0xc3d2e1f0;
}

/*
 * pg_sha1_update
 *
 * Update a SHA1 context.
 */
void
pg_sha1_update(pg_sha1_ctx *ctx, const uint8 *data, size_t len)
{
	const uint8 *input;
	size_t		gaplen;
	size_t		gapstart;
	size_t		off;
	size_t		copysiz;

	input = (const uint8 *) data;
	off = 0;

	while (off < len)
	{
		gapstart = COUNT % 64;
		gaplen = 64 - gapstart;

		copysiz = (gaplen < len - off) ? gaplen : len - off;
		memmove(&ctx->m.b8[gapstart], &input[off], copysiz);
		COUNT += copysiz;
		COUNT %= 64;
		ctx->c.b64[0] += copysiz * 8;
		if (COUNT % 64 == 0)
			sha1_step(ctx);
		off += copysiz;
	}
}

/*
 * pg_sha1_final
 *
 * Finalize a SHA1 context.
 */
void
pg_sha1_final(pg_sha1_ctx *ctx, uint8 *dest)
{
	sha1_pad(ctx);
	sha1_result(dest, ctx);
}

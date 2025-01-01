/*-------------------------------------------------------------------------
 *
 * sha2_int.h
 *	  Internal headers for fallback implementation of SHA{224,256,384,512}
 *
 * Portions Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/common/sha2_int.h
 *
 *-------------------------------------------------------------------------
 */

/* $OpenBSD: sha2.h,v 1.2 2004/04/28 23:11:57 millert Exp $ */

/*
 * FILE:	sha2.h
 * AUTHOR:	Aaron D. Gifford <me@aarongifford.com>
 *
 * Copyright (c) 2000-2001, Aaron D. Gifford
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
 * 3. Neither the name of the copyright holder nor the names of contributors
 *	  may be used to endorse or promote products derived from this software
 *	  without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTOR(S) ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTOR(S) BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $From: sha2.h,v 1.1 2001/11/08 00:02:01 adg Exp adg $
 */

#ifndef PG_SHA2_INT_H
#define PG_SHA2_INT_H

#include "common/sha2.h"

typedef struct pg_sha256_ctx
{
	uint32		state[8];
	uint64		bitcount;
	uint8		buffer[PG_SHA256_BLOCK_LENGTH];
} pg_sha256_ctx;
typedef struct pg_sha512_ctx
{
	uint64		state[8];
	uint64		bitcount[2];
	uint8		buffer[PG_SHA512_BLOCK_LENGTH];
} pg_sha512_ctx;
typedef struct pg_sha256_ctx pg_sha224_ctx;
typedef struct pg_sha512_ctx pg_sha384_ctx;

/* Interface routines for SHA224/256/384/512 */
extern void pg_sha224_init(pg_sha224_ctx *ctx);
extern void pg_sha224_update(pg_sha224_ctx *ctx, const uint8 *input0,
							 size_t len);
extern void pg_sha224_final(pg_sha224_ctx *ctx, uint8 *dest);

extern void pg_sha256_init(pg_sha256_ctx *ctx);
extern void pg_sha256_update(pg_sha256_ctx *ctx, const uint8 *input0,
							 size_t len);
extern void pg_sha256_final(pg_sha256_ctx *ctx, uint8 *dest);

extern void pg_sha384_init(pg_sha384_ctx *ctx);
extern void pg_sha384_update(pg_sha384_ctx *ctx,
							 const uint8 *, size_t len);
extern void pg_sha384_final(pg_sha384_ctx *ctx, uint8 *dest);

extern void pg_sha512_init(pg_sha512_ctx *ctx);
extern void pg_sha512_update(pg_sha512_ctx *ctx, const uint8 *input0,
							 size_t len);
extern void pg_sha512_final(pg_sha512_ctx *ctx, uint8 *dest);

#endif							/* PG_SHA2_INT_H */

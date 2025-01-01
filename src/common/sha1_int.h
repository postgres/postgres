/*-------------------------------------------------------------------------
 *
 * sha1_int.h
 *	  Internal headers for fallback implementation of SHA1
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		  src/common/sha1_int.h
 *
 *-------------------------------------------------------------------------
 */

/*	   $KAME: sha1.h,v 1.4 2000/02/22 14:01:18 itojun Exp $    */

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

#ifndef PG_SHA1_INT_H
#define PG_SHA1_INT_H

#include "common/sha1.h"

typedef struct
{
	union
	{
		uint8		b8[20];
		uint32		b32[5];
	}			h;
	union
	{
		uint8		b8[8];
		uint64		b64[1];
	}			c;
	union
	{
		uint8		b8[64];
		uint32		b32[16];
	}			m;
	uint8		count;
} pg_sha1_ctx;

/* Interface routines for SHA1 */
extern void pg_sha1_init(pg_sha1_ctx *ctx);
extern void pg_sha1_update(pg_sha1_ctx *ctx, const uint8 *data, size_t len);
extern void pg_sha1_final(pg_sha1_ctx *ctx, uint8 *dest);

#endif							/* PG_SHA1_INT_H */

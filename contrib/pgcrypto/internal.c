/*
 * internal.c
 *		Wrapper for builtin functions
 *
 * Copyright (c) 2000 Marko Kreen
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
 * $Id: internal.c,v 1.3 2001/03/22 03:59:10 momjian Exp $
 */

#include "postgres.h"

#include "pgcrypto.h"

#include "md5.h"
#include "sha1.h"

#ifndef MD5_DIGEST_LENGTH
#define MD5_DIGEST_LENGTH 16
#endif

#ifndef SHA1_DIGEST_LENGTH
#ifdef SHA1_RESULTLEN
#define SHA1_DIGEST_LENGTH SHA1_RESULTLEN
#else
#define SHA1_DIGEST_LENGTH 20
#endif
#endif

static uint
			pg_md5_len(pg_digest * h);
static uint8 *
			pg_md5_digest(pg_digest * h, uint8 *src, uint len, uint8 *buf);

static uint
			pg_sha1_len(pg_digest * h);
static uint8 *
			pg_sha1_digest(pg_digest * h, uint8 *src, uint len, uint8 *buf);

static pg_digest
			int_digest_list[] = {
	{"md5", pg_md5_len, pg_md5_digest, {0}},
	{"sha1", pg_sha1_len, pg_sha1_digest, {0}},
	{NULL, NULL, NULL, {0}}
};

static uint
pg_md5_len(pg_digest * h)
{
	return MD5_DIGEST_LENGTH;
}

static uint8 *
pg_md5_digest(pg_digest * h, uint8 *src, uint len, uint8 *buf)
{
	MD5_CTX		ctx;

	MD5Init(&ctx);
	MD5Update(&ctx, src, len);
	MD5Final(buf, &ctx);

	return buf;
}

static uint
pg_sha1_len(pg_digest * h)
{
	return SHA1_DIGEST_LENGTH;
}

static uint8 *
pg_sha1_digest(pg_digest * h, uint8 *src, uint len, uint8 *buf)
{
	SHA1_CTX	ctx;

	SHA1Init(&ctx);
	SHA1Update(&ctx, src, len);
	SHA1Final(buf, &ctx);

	return buf;
}


pg_digest  *
pg_find_digest(pg_digest * h, char *name)
{
	pg_digest  *p;

	for (p = int_digest_list; p->name; p++)
		if (!strcasecmp(p->name, name))
			return p;
	return NULL;
}

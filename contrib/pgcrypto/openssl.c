/*
 * openssl.c
 *		Wrapper for OpenSSL library.
 * 
 * Copyright (c) 2000 Marko Kreen
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
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
 * $Id: openssl.c,v 1.2 2001/02/10 02:31:26 tgl Exp $
 */

#include "postgres.h"

#include "pgcrypto.h"

#include <evp.h>

static uint
pg_ossl_len(pg_digest *h);
static uint8 *
pg_ossl_digest(pg_digest *h, uint8 *src, uint len, uint8 *buf);

static uint
pg_ossl_len(pg_digest *h) {
	return EVP_MD_size((EVP_MD*)h->misc.ptr);
}

static uint8 *
pg_ossl_digest(pg_digest *h, uint8 *src, uint len, uint8 *buf)
{
	EVP_MD *md = (EVP_MD*)h->misc.ptr;
	EVP_MD_CTX ctx;

	EVP_DigestInit(&ctx, md);
	EVP_DigestUpdate(&ctx, src, len);
	EVP_DigestFinal(&ctx, buf, NULL);
	
	return buf;
}

static int pg_openssl_initialized = 0;

pg_digest *
pg_find_digest(pg_digest *h, char *name)
{
	const EVP_MD *md;

	if (!pg_openssl_initialized) {
		OpenSSL_add_all_digests();
		pg_openssl_initialized = 1;
	}
	
	md = EVP_get_digestbyname(name);
	if (md == NULL)
		return NULL;
	
	h->name = name;
	h->length = pg_ossl_len;
	h->digest = pg_ossl_digest;
	h->misc.ptr = (void*)md;
	
	return h;
}



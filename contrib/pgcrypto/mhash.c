/*
 * mhash.c
 *		Wrapper for mhash and mcrypt libraries.
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
 * ARE DISCLAIMED.	IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id: mhash.c,v 1.8 2003/07/24 17:52:33 tgl Exp $
 */

#include <postgres.h>

#include "px.h"

#include <mhash.h>
#include <mcrypt.h>

#define MAX_KEY_LENGTH 512
#define MAX_IV_LENGTH 128

#define DEF_KEY_LEN 16


/* DIGEST */

static unsigned
digest_result_size(PX_MD * h)
{
	MHASH		mh = (MHASH) h->p.ptr;
	hashid		id = mhash_get_mhash_algo(mh);

	return mhash_get_block_size(id);
}

static unsigned
digest_block_size(PX_MD * h)
{
	MHASH		mh = (MHASH) h->p.ptr;
	hashid		id = mhash_get_mhash_algo(mh);

	return mhash_get_hash_pblock(id);
}

static void
digest_reset(PX_MD * h)
{
	MHASH		mh = (MHASH) h->p.ptr;
	hashid		id = mhash_get_mhash_algo(mh);
	uint8	   *res = mhash_end(mh);

	mhash_free(res);
	mh = mhash_init(id);
	h->p.ptr = mh;
}

static void
digest_update(PX_MD * h, const uint8 *data, unsigned dlen)
{
	MHASH		mh = (MHASH) h->p.ptr;

	mhash(mh, data, dlen);
}

static void
digest_finish(PX_MD * h, uint8 *dst)
{
	MHASH		mh = (MHASH) h->p.ptr;
	unsigned	hlen = digest_result_size(h);
	hashid		id = mhash_get_mhash_algo(mh);
	uint8	   *buf = mhash_end(mh);

	memcpy(dst, buf, hlen);
	mhash_free(buf);

	mh = mhash_init(id);
	h->p.ptr = mh;
}

static void
digest_free(PX_MD * h)
{
	MHASH		mh = (MHASH) h->p.ptr;
	uint8	   *buf = mhash_end(mh);

	mhash_free(buf);

	px_free(h);
}

/* ENCRYPT / DECRYPT */

static unsigned
cipher_block_size(PX_Cipher * c)
{
	MCRYPT		ctx = (MCRYPT) c->ptr;

	return mcrypt_enc_get_block_size(ctx);
}

static unsigned
cipher_key_size(PX_Cipher * c)
{
	MCRYPT		ctx = (MCRYPT) c->ptr;

	return mcrypt_enc_get_key_size(ctx);
}

static unsigned
cipher_iv_size(PX_Cipher * c)
{
	MCRYPT		ctx = (MCRYPT) c->ptr;

	return mcrypt_enc_mode_has_iv(ctx)
		? mcrypt_enc_get_iv_size(ctx) : 0;
}

static int
cipher_init(PX_Cipher * c, const uint8 *key, unsigned klen, const uint8 *iv)
{
	int			err;
	MCRYPT		ctx = (MCRYPT) c->ptr;

	err = mcrypt_generic_init(ctx, (char *) key, klen, (char *) iv);
	if (err < 0)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
				 errmsg("mcrypt_generic_init error"),
				 errdetail("%s", mcrypt_strerror(err))));

	c->pstat = 1;
	return 0;
}

static int
cipher_encrypt(PX_Cipher * c, const uint8 *data, unsigned dlen, uint8 *res)
{
	int			err;
	MCRYPT		ctx = (MCRYPT) c->ptr;

	memcpy(res, data, dlen);

	err = mcrypt_generic(ctx, res, dlen);
	if (err < 0)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
				 errmsg("mcrypt_generic error"),
				 errdetail("%s", mcrypt_strerror(err))));
	return 0;
}

static int
cipher_decrypt(PX_Cipher * c, const uint8 *data, unsigned dlen, uint8 *res)
{
	int			err;
	MCRYPT		ctx = (MCRYPT) c->ptr;

	memcpy(res, data, dlen);

	err = mdecrypt_generic(ctx, res, dlen);
	if (err < 0)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
				 errmsg("mdecrypt_generic error"),
				 errdetail("%s", mcrypt_strerror(err))));
	return 0;
}


static void
cipher_free(PX_Cipher * c)
{
	MCRYPT		ctx = (MCRYPT) c->ptr;

	if (c->pstat)
		mcrypt_generic_end(ctx);
	else
		mcrypt_module_close(ctx);

	px_free(c);
}

/* Helper functions */

static int
find_hashid(const char *name)
{
	int			res = -1;
	size_t		hnum,
				b,
				i;
	char	   *mname;

	hnum = mhash_count();
	for (i = 0; i <= hnum; i++)
	{
		mname = mhash_get_hash_name(i);
		if (mname == NULL)
			continue;
		b = strcasecmp(name, mname);
		free(mname);
		if (!b)
		{
			res = i;
			break;
		}
	}

	return res;
}

static char *modes[] = {
	"ecb", "cbc", "cfb", "ofb", "nofb", "stream",
	"ofb64", "cfb64", NULL
};

static PX_Alias aliases[] = {
	{"bf", "blowfish"},
	{"3des", "tripledes"},
	{"des3", "tripledes"},
	{"aes", "rijndael-128"},
	{"rijndael", "rijndael-128"},
	{"aes-128", "rijndael-128"},
	{"aes-192", "rijndael-192"},
	{"aes-256", "rijndael-256"},
	{NULL, NULL}
};

static PX_Alias mode_aliases[] = {
#if 0							/* N/A */
	{"cfb", "ncfb"},
	{"ofb", "nofb"},
	{"cfb64", "ncfb"},
#endif
	/* { "ofb64", "nofb" }, not sure it works */
	{"cfb8", "cfb"},
	{"ofb8", "ofb"},
	{NULL, NULL}
};

static int
is_mode(char *s)
{
	char	  **p;

	if (*s >= '0' && *s <= '9')
		return 0;

	for (p = modes; *p; p++)
		if (!strcmp(s, *p))
			return 1;

	return 0;
}

/* PUBLIC FUNCTIONS */

int
px_find_digest(const char *name, PX_MD ** res)
{
	PX_MD	   *h;
	MHASH		mh;
	int			i;

	i = find_hashid(name);
	if (i < 0)
		return -1;

	mh = mhash_init(i);
	h = px_alloc(sizeof(*h));
	h->p.ptr = (void *) mh;

	h->result_size = digest_result_size;
	h->block_size = digest_block_size;
	h->reset = digest_reset;
	h->update = digest_update;
	h->finish = digest_finish;
	h->free = digest_free;

	*res = h;
	return 0;
}


int
px_find_cipher(const char *name, PX_Cipher ** res)
{
	char		nbuf[PX_MAX_NAMELEN + 1];
	const char *mode = NULL;
	char	   *p;
	MCRYPT		ctx;

	PX_Cipher  *c;

	strcpy(nbuf, name);

	if ((p = strrchr(nbuf, '-')) != NULL)
	{
		if (is_mode(p + 1))
		{
			mode = p + 1;
			*p = 0;
		}
	}

	name = px_resolve_alias(aliases, nbuf);

	if (!mode)
	{
		mode = "cbc";

		/*
		 * if (mcrypt_module_is_block_algorithm(name, NULL)) mode = "cbc";
		 * else mode = "stream";
		 */
	}
	mode = px_resolve_alias(mode_aliases, mode);

	ctx = mcrypt_module_open((char *) name, NULL, (char *) mode, NULL);
	if (ctx == (void *) MCRYPT_FAILED)
		return -1;

	c = palloc(sizeof *c);
	c->iv_size = cipher_iv_size;
	c->key_size = cipher_key_size;
	c->block_size = cipher_block_size;
	c->init = cipher_init;
	c->encrypt = cipher_encrypt;
	c->decrypt = cipher_decrypt;
	c->free = cipher_free;
	c->ptr = ctx;
	c->pstat = 0;

	*res = c;
	return 0;
}

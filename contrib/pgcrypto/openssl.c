/*
 * openssl.c
 *		Wrapper for OpenSSL library.
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
 * $Id: openssl.c,v 1.10 2001/11/20 18:54:07 momjian Exp $
 */

#include <postgres.h>

#include "px.h"

#include <openssl/evp.h>
#include <openssl/blowfish.h>

static unsigned
digest_result_size(PX_MD * h)
{
	return EVP_MD_CTX_size((EVP_MD_CTX *) h->p.ptr);
}

static unsigned
digest_block_size(PX_MD * h)
{
	return EVP_MD_CTX_block_size((EVP_MD_CTX *) h->p.ptr);
}

static void
digest_reset(PX_MD * h)
{
	EVP_MD_CTX *ctx = (EVP_MD_CTX *) h->p.ptr;
	const EVP_MD *md;

	md = EVP_MD_CTX_md(ctx);

	EVP_DigestInit(ctx, md);
}

static void
digest_update(PX_MD * h, const uint8 *data, unsigned dlen)
{
	EVP_MD_CTX *ctx = (EVP_MD_CTX *) h->p.ptr;

	EVP_DigestUpdate(ctx, data, dlen);
}

static void
digest_finish(PX_MD * h, uint8 *dst)
{
	EVP_MD_CTX *ctx = (EVP_MD_CTX *) h->p.ptr;

	EVP_DigestFinal(ctx, dst, NULL);
}

static void
digest_free(PX_MD * h)
{
	EVP_MD_CTX *ctx = (EVP_MD_CTX *) h->p.ptr;

	px_free(ctx);
	px_free(h);
}

/* CIPHERS */

/*
 * The problem with OpenSSL is that the EVP* family
 * of functions does not allow enough flexibility
 * and forces some of the parameters (keylen,
 * padding) to SSL defaults.
 */


typedef struct
{
	union
	{
		struct
		{
			BF_KEY		key;
			int			num;
		}			bf;
		EVP_CIPHER_CTX evp_ctx;
	}			u;
	const EVP_CIPHER *evp_ciph;
	uint8		key[EVP_MAX_KEY_LENGTH];
	uint8		iv[EVP_MAX_IV_LENGTH];
	unsigned	klen;
	unsigned	init;
}	ossldata;

/* generic EVP */

static unsigned
gen_evp_block_size(PX_Cipher * c)
{
	ossldata   *od = (ossldata *) c->ptr;

	return EVP_CIPHER_block_size(od->evp_ciph);
}

static unsigned
gen_evp_key_size(PX_Cipher * c)
{
	ossldata   *od = (ossldata *) c->ptr;

	return EVP_CIPHER_key_length(od->evp_ciph);
}

static unsigned
gen_evp_iv_size(PX_Cipher * c)
{
	unsigned	ivlen;
	ossldata   *od = (ossldata *) c->ptr;

	ivlen = EVP_CIPHER_iv_length(od->evp_ciph);
	return ivlen;
}

static void
gen_evp_free(PX_Cipher * c)
{
	ossldata   *od = (ossldata *) c->ptr;

	memset(od, 0, sizeof(*od));
	pfree(od);
	pfree(c);
}

/* fun */

static int
gen_evp_init(PX_Cipher * c, const uint8 *key, unsigned klen, const uint8 *iv)
{
	ossldata   *od = (ossldata *) c->ptr;
	unsigned	bs = gen_evp_block_size(c);

	if (iv)
		memcpy(od->iv, iv, bs);
	else
		memset(od->iv, 0, bs);
	memcpy(od->key, key, klen);
	od->klen = klen;
	od->init = 0;
	return 0;
}

static void
_gen_init(PX_Cipher * c, int enc)
{
	ossldata   *od = c->ptr;

	od->evp_ciph->init(&od->u.evp_ctx, od->key, od->iv, enc);
	od->init = 1;
	od->u.evp_ctx.encrypt = enc;
}

static int
gen_evp_encrypt(PX_Cipher * c, const uint8 *data, unsigned dlen, uint8 *res)
{
	ossldata   *od = c->ptr;

	if (!od->init)
		_gen_init(c, 1);
	od->evp_ciph->do_cipher(&od->u.evp_ctx, res, data, dlen);
	return 0;
}

static int
gen_evp_decrypt(PX_Cipher * c, const uint8 *data, unsigned dlen, uint8 *res)
{
	ossldata   *od = c->ptr;

	if (!od->init)
		_gen_init(c, 0);
	od->evp_ciph->do_cipher(&od->u.evp_ctx, res, data, dlen);
	return 0;
}

/* Blowfish */

static int
bf_init(PX_Cipher * c, const uint8 *key, unsigned klen, const uint8 *iv)
{
	ossldata   *od = c->ptr;

	BF_set_key(&od->u.bf.key, klen, key);
	if (iv)
		memcpy(od->iv, iv, BF_BLOCK);
	else
		memset(od->iv, 0, BF_BLOCK);
	od->u.bf.num = 0;
	return 0;
}

static int
bf_ecb_encrypt(PX_Cipher * c, const uint8 *data, unsigned dlen, uint8 *res)
{
	unsigned	bs = gen_evp_block_size(c),
				i;
	ossldata   *od = c->ptr;

	for (i = 0; i < dlen / bs; i++)
		BF_ecb_encrypt(data + i * bs, res + i * bs, &od->u.bf.key, BF_ENCRYPT);
	return 0;
}

static int
bf_ecb_decrypt(PX_Cipher * c, const uint8 *data, unsigned dlen, uint8 *res)
{
	unsigned	bs = gen_evp_block_size(c),
				i;
	ossldata   *od = c->ptr;

	for (i = 0; i < dlen / bs; i++)
		BF_ecb_encrypt(data + i * bs, res + i * bs, &od->u.bf.key, BF_DECRYPT);
	return 0;
}

static int
bf_cbc_encrypt(PX_Cipher * c, const uint8 *data, unsigned dlen, uint8 *res)
{
	ossldata   *od = c->ptr;

	BF_cbc_encrypt(data, res, dlen, &od->u.bf.key, od->iv, BF_ENCRYPT);
	return 0;
}

static int
bf_cbc_decrypt(PX_Cipher * c, const uint8 *data, unsigned dlen, uint8 *res)
{
	ossldata   *od = c->ptr;

	BF_cbc_encrypt(data, res, dlen, &od->u.bf.key, od->iv, BF_DECRYPT);
	return 0;
}

static int
bf_cfb64_encrypt(PX_Cipher * c, const uint8 *data, unsigned dlen, uint8 *res)
{
	ossldata   *od = c->ptr;

	BF_cfb64_encrypt(data, res, dlen, &od->u.bf.key, od->iv,
					 &od->u.bf.num, BF_ENCRYPT);
	return 0;
}

static int
bf_cfb64_decrypt(PX_Cipher * c, const uint8 *data, unsigned dlen, uint8 *res)
{
	ossldata   *od = c->ptr;

	BF_cfb64_encrypt(data, res, dlen, &od->u.bf.key, od->iv,
					 &od->u.bf.num, BF_DECRYPT);
	return 0;
}

static int
bf_ofb64_encrypt(PX_Cipher * c, const uint8 *data, unsigned dlen, uint8 *res)
{
	ossldata   *od = c->ptr;

	BF_ofb64_encrypt(data, res, dlen, &od->u.bf.key, od->iv, &od->u.bf.num);
	return 0;
}

static int
bf_ofb64_decrypt(PX_Cipher * c, const uint8 *data, unsigned dlen, uint8 *res)
{
	ossldata   *od = c->ptr;

	BF_ofb64_encrypt(data, res, dlen, &od->u.bf.key, od->iv, &od->u.bf.num);
	return 0;
}

/*
 * aliases
 */

static PX_Alias ossl_aliases[] = {
	{"bf", "bf-cbc"},
	{"blowfish", "bf-cbc"},
	{"blowfish-cbc", "bf-cbc"},
	{"blowfish-ecb", "bf-ecb"},
	{"blowfish-cfb", "bf-cfb"},
	{"blowfish-ofb", "bf-ofb"},
	{NULL}
};

/*
static PX_Alias ossl_mode_aliases [] = {
	{ "cfb64", "cfb" },
	{ "ofb64", "ofb" },
	{ NULL }
};*/

/*
 * Special handlers
 */
struct
{
	char	   *name;
	PX_Cipher	cf;
}	spec_types[] =

{
	{
		"bf-cbc",
		{
			gen_evp_block_size, gen_evp_key_size, gen_evp_iv_size,
			bf_init, bf_cbc_encrypt, bf_cbc_decrypt, gen_evp_free
		}
	},
	{
		"bf-ecb",
		{
			gen_evp_block_size, gen_evp_key_size, gen_evp_iv_size,
			bf_init, bf_ecb_encrypt, bf_ecb_decrypt, gen_evp_free
		}
	},
	{
		"bf-cfb",
		{
			gen_evp_block_size, gen_evp_key_size, gen_evp_iv_size,
			bf_init, bf_cfb64_encrypt, bf_cfb64_decrypt, gen_evp_free
		}
	},
	{
		"bf-ofb",
		{
			gen_evp_block_size, gen_evp_key_size, gen_evp_iv_size,
			bf_init, bf_ofb64_encrypt, bf_ofb64_decrypt, gen_evp_free
		}
	},
	{
		NULL
	}
};

/*
 * Generic EVP_* functions handler
 */
static PX_Cipher gen_evp_handler = {
	gen_evp_block_size, gen_evp_key_size, gen_evp_iv_size,
	gen_evp_init, gen_evp_encrypt, gen_evp_decrypt, gen_evp_free
};

static int	px_openssl_initialized = 0;

/* ATM not needed
static void *o_alloc(unsigned s) { return px_alloc(s); }
static void *o_realloc(void *p) { return px_realloc(p); }
static void o_free(void *p) { px_free(p); }
*/

/* PUBLIC functions */

int
px_find_digest(const char *name, PX_MD ** res)
{
	const EVP_MD *md;
	EVP_MD_CTX *ctx;
	PX_MD	   *h;

	if (!px_openssl_initialized)
	{
		px_openssl_initialized = 1;
		/* CRYPTO_set_mem_functions(o_alloc, o_realloc, o_free); */
		OpenSSL_add_all_algorithms();
	}

	md = EVP_get_digestbyname(name);
	if (md == NULL)
		return -1;

	ctx = px_alloc(sizeof(*ctx));
	EVP_DigestInit(ctx, md);

	h = px_alloc(sizeof(*h));
	h->result_size = digest_result_size;
	h->block_size = digest_block_size;
	h->reset = digest_reset;
	h->update = digest_update;
	h->finish = digest_finish;
	h->free = digest_free;
	h->p.ptr = (void *) ctx;

	*res = h;
	return 0;
}


int
px_find_cipher(const char *name, PX_Cipher ** res)
{
	unsigned	i;
	PX_Cipher  *c = NULL,
			   *csrc;
	ossldata   *od;

	const EVP_CIPHER *evp_c;

	if (!px_openssl_initialized)
	{
		px_openssl_initialized = 1;
		/* CRYPTO_set_mem_functions(o_alloc, o_realloc, o_free); */
		OpenSSL_add_all_algorithms();
	}

	name = px_resolve_alias(ossl_aliases, name);
	evp_c = EVP_get_cipherbyname(name);
	if (evp_c == NULL)
		return -1;

	od = px_alloc(sizeof(*od));
	memset(od, 0, sizeof(*od));
	od->evp_ciph = evp_c;

	csrc = NULL;

	for (i = 0; spec_types[i].name; i++)
		if (!strcmp(name, spec_types[i].name))
		{
			csrc = &spec_types[i].cf;
			break;
		}

	if (csrc == NULL)
		csrc = &gen_evp_handler;

	c = px_alloc(sizeof(*c));
	memcpy(c, csrc, sizeof(*c));
	c->ptr = od;

	*res = c;
	return 0;
}

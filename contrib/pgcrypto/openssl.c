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
 * $Id: openssl.c,v 1.12.4.2 2006/02/18 20:49:00 neilc Exp $
 */

#include <postgres.h>

#include "px.h"

#include <openssl/evp.h>

/*
 * Backwards compatibility code for digest.
 */
#if OPENSSL_VERSION_NUMBER < 0x00907000L

static void EVP_MD_CTX_init(EVP_MD_CTX *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
}

static int EVP_MD_CTX_cleanup(EVP_MD_CTX *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	return 1;
}

static int EVP_DigestInit_ex(EVP_MD_CTX *ctx, const EVP_MD *md, void *engine)
{
	EVP_DigestInit(ctx, md);
	return 1;
}

static int EVP_DigestFinal_ex(EVP_MD_CTX *ctx, unsigned char *res, unsigned int *len)
{
	EVP_DigestFinal(ctx, res, len);
	return 1;
}

#endif

/*
 * Hashes
 */

typedef struct OSSLDigest {
	const EVP_MD *algo;
	EVP_MD_CTX ctx;
} OSSLDigest;

static unsigned
digest_result_size(PX_MD * h)
{
	OSSLDigest *digest = (OSSLDigest *)h->p.ptr;
	return EVP_MD_CTX_size(&digest->ctx);
}

static unsigned
digest_block_size(PX_MD * h)
{
	OSSLDigest *digest = (OSSLDigest *)h->p.ptr;
	return EVP_MD_CTX_block_size(&digest->ctx);
}

static void
digest_reset(PX_MD * h)
{
	OSSLDigest *digest = (OSSLDigest *)h->p.ptr;

	EVP_DigestInit_ex(&digest->ctx, digest->algo, NULL);
}

static void
digest_update(PX_MD * h, const uint8 *data, unsigned dlen)
{
	OSSLDigest *digest = (OSSLDigest *)h->p.ptr;

	EVP_DigestUpdate(&digest->ctx, data, dlen);
}

static void
digest_finish(PX_MD * h, uint8 *dst)
{
	OSSLDigest *digest = (OSSLDigest *)h->p.ptr;

	EVP_DigestFinal_ex(&digest->ctx, dst, NULL);
}

static void
digest_free(PX_MD * h)
{
	OSSLDigest *digest = (OSSLDigest *)h->p.ptr;

	EVP_MD_CTX_cleanup(&digest->ctx);
	px_free(digest);
	px_free(h);
}

static int	px_openssl_initialized = 0;

/* PUBLIC functions */

int
px_find_digest(const char *name, PX_MD ** res)
{
	const EVP_MD *md;
	PX_MD	   *h;
	OSSLDigest *digest;

	if (!px_openssl_initialized)
	{
		px_openssl_initialized = 1;
		OpenSSL_add_all_algorithms();
	}

	md = EVP_get_digestbyname(name);
	if (md == NULL)
		return -1;

	digest = px_alloc(sizeof(*digest));
	digest->algo = md;
	
	EVP_MD_CTX_init(&digest->ctx);
	if (EVP_DigestInit_ex(&digest->ctx, digest->algo, NULL) == 0)
		return -1;

	h = px_alloc(sizeof(*h));
	h->result_size = digest_result_size;
	h->block_size = digest_block_size;
	h->reset = digest_reset;
	h->update = digest_update;
	h->finish = digest_finish;
	h->free = digest_free;
	h->p.ptr = (void *) digest;

	*res = h;
	return 0;
}

/*
 * Ciphers
 *
 * The problem with OpenSSL is that the EVP* family
 * of functions does not allow enough flexibility
 * and forces some of the parameters (keylen,
 * padding) to SSL defaults.
 *
 * So need to manage ciphers ourselves.
 */

struct ossl_cipher
{
	int			(*init) (PX_Cipher * c, const uint8 *key, unsigned klen, const uint8 *iv);
	int			(*encrypt) (PX_Cipher * c, const uint8 *data, unsigned dlen, uint8 *res);
	int			(*decrypt) (PX_Cipher * c, const uint8 *data, unsigned dlen, uint8 *res);

	int			block_size;
	int			max_key_size;
	int			stream_cipher;
};

typedef struct
{
	union
	{
		struct
		{
			BF_KEY		key;
			int			num;
		}			bf;
		struct
		{
			des_key_schedule key_schedule;
		}			des;
		CAST_KEY	cast_key;
	}			u;
	uint8		key[EVP_MAX_KEY_LENGTH];
	uint8		iv[EVP_MAX_IV_LENGTH];
	unsigned	klen;
	unsigned	init;
	const struct ossl_cipher *ciph;
}	ossldata;

/* generic */

static unsigned
gen_ossl_block_size(PX_Cipher * c)
{
	ossldata   *od = (ossldata *) c->ptr;

	return od->ciph->block_size;
}

static unsigned
gen_ossl_key_size(PX_Cipher * c)
{
	ossldata   *od = (ossldata *) c->ptr;

	return od->ciph->max_key_size;
}

static unsigned
gen_ossl_iv_size(PX_Cipher * c)
{
	unsigned	ivlen;
	ossldata   *od = (ossldata *) c->ptr;

	ivlen = od->ciph->block_size;
	return ivlen;
}

static void
gen_ossl_free(PX_Cipher * c)
{
	ossldata   *od = (ossldata *) c->ptr;

	memset(od, 0, sizeof(*od));
	pfree(od);
	pfree(c);
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
	unsigned	bs = gen_ossl_block_size(c);
	unsigned	i;
	ossldata   *od = c->ptr;

	for (i = 0; i < dlen / bs; i++)
		BF_ecb_encrypt(data + i * bs, res + i * bs, &od->u.bf.key, BF_ENCRYPT);
	return 0;
}

static int
bf_ecb_decrypt(PX_Cipher * c, const uint8 *data, unsigned dlen, uint8 *res)
{
	unsigned	bs = gen_ossl_block_size(c),
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

/* DES */

static int
ossl_des_init(PX_Cipher * c, const uint8 *key, unsigned klen, const uint8 *iv)
{
	ossldata   *od = c->ptr;
	des_cblock	xkey;

	memset(&xkey, 0, sizeof(xkey));
	memcpy(&xkey, key, klen > 8 ? 8 : klen);
	des_set_key(&xkey, od->u.des.key_schedule);
	memset(&xkey, 0, sizeof(xkey));

	if (iv)
		memcpy(od->iv, iv, 8);
	else
		memset(od->iv, 0, 8);
	return 0;
}

static int
ossl_des_ecb_encrypt(PX_Cipher * c, const uint8 *data, unsigned dlen,
					 uint8 *res)
{
	unsigned	bs = gen_ossl_block_size(c);
	unsigned	i;
	ossldata   *od = c->ptr;

	for (i = 0; i < dlen / bs; i++)
		des_ecb_encrypt((des_cblock *) (data + i * bs),
						(des_cblock *) (res + i * bs),
						od->u.des.key_schedule, 1);
	return 0;
}

static int
ossl_des_ecb_decrypt(PX_Cipher * c, const uint8 *data, unsigned dlen,
					 uint8 *res)
{
	unsigned	bs = gen_ossl_block_size(c);
	unsigned	i;
	ossldata   *od = c->ptr;

	for (i = 0; i < dlen / bs; i++)
		des_ecb_encrypt((des_cblock *) (data + i * bs),
						(des_cblock *) (res + i * bs),
						od->u.des.key_schedule, 0);
	return 0;
}

static int
ossl_des_cbc_encrypt(PX_Cipher * c, const uint8 *data, unsigned dlen,
					 uint8 *res)
{
	ossldata   *od = c->ptr;

	des_ncbc_encrypt(data, res, dlen, od->u.des.key_schedule,
					 (des_cblock *) od->iv, 1);
	return 0;
}

static int
ossl_des_cbc_decrypt(PX_Cipher * c, const uint8 *data, unsigned dlen,
					 uint8 *res)
{
	ossldata   *od = c->ptr;

	des_ncbc_encrypt(data, res, dlen, od->u.des.key_schedule,
					 (des_cblock *) od->iv, 0);
	return 0;
}

/* CAST5 */

static int
ossl_cast_init(PX_Cipher * c, const uint8 *key, unsigned klen, const uint8 *iv)
{
	ossldata   *od = c->ptr;
	unsigned	bs = gen_ossl_block_size(c);

	CAST_set_key(&od->u.cast_key, klen, key);
	if (iv)
		memcpy(od->iv, iv, bs);
	else
		memset(od->iv, 0, bs);
	return 0;
}

static int
ossl_cast_ecb_encrypt(PX_Cipher * c, const uint8 *data, unsigned dlen, uint8 *res)
{
	unsigned	bs = gen_ossl_block_size(c);
	ossldata   *od = c->ptr;
	const uint8 *end = data + dlen - bs;

	for (; data <= end; data += bs, res += bs)
		CAST_ecb_encrypt(data, res, &od->u.cast_key, CAST_ENCRYPT);
	return 0;
}

static int
ossl_cast_ecb_decrypt(PX_Cipher * c, const uint8 *data, unsigned dlen, uint8 *res)
{
	unsigned	bs = gen_ossl_block_size(c);
	ossldata   *od = c->ptr;
	const uint8 *end = data + dlen - bs;

	for (; data <= end; data += bs, res += bs)
		CAST_ecb_encrypt(data, res, &od->u.cast_key, CAST_DECRYPT);
	return 0;
}

static int
ossl_cast_cbc_encrypt(PX_Cipher * c, const uint8 *data, unsigned dlen, uint8 *res)
{
	ossldata   *od = c->ptr;

	CAST_cbc_encrypt(data, res, dlen, &od->u.cast_key, od->iv, CAST_ENCRYPT);
	return 0;
}

static int
ossl_cast_cbc_decrypt(PX_Cipher * c, const uint8 *data, unsigned dlen, uint8 *res)
{
	ossldata   *od = c->ptr;

	CAST_cbc_encrypt(data, res, dlen, &od->u.cast_key, od->iv, CAST_DECRYPT);
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
	{"des", "des-cbc"},
	{"cast5", "cast5-cbc"},
	{NULL}
};

static const struct ossl_cipher ossl_bf_cbc = {
	bf_init, bf_cbc_encrypt, bf_cbc_decrypt,
	64 / 8, 448 / 8, 0
};

static const struct ossl_cipher ossl_bf_ecb = {
	bf_init, bf_ecb_encrypt, bf_ecb_decrypt,
	64 / 8, 448 / 8, 0
};

static const struct ossl_cipher ossl_bf_cfb = {
	bf_init, bf_cfb64_encrypt, bf_cfb64_decrypt,
	64 / 8, 448 / 8, 1
};

static const struct ossl_cipher ossl_des_ecb = {
	ossl_des_init, ossl_des_ecb_encrypt, ossl_des_ecb_decrypt,
	64 / 8, 64 / 8, 0
};

static const struct ossl_cipher ossl_des_cbc = {
	ossl_des_init, ossl_des_cbc_encrypt, ossl_des_cbc_decrypt,
	64 / 8, 64 / 8, 0
};

static const struct ossl_cipher ossl_cast_ecb = {
	ossl_cast_init, ossl_cast_ecb_encrypt, ossl_cast_ecb_decrypt,
	64 / 8, 128 / 8, 0
};

static const struct ossl_cipher ossl_cast_cbc = {
	ossl_cast_init, ossl_cast_cbc_encrypt, ossl_cast_cbc_decrypt,
	64 / 8, 128 / 8, 0
};

/*
 * Special handlers
 */
static const struct
{
	const char *name;
	const struct ossl_cipher *ciph;
}	ossl_cipher_types[] =

{
	{
		"bf-cbc", &ossl_bf_cbc
	},
	{
		"bf-ecb", &ossl_bf_ecb
	},
	{
		"bf-cfb", &ossl_bf_cfb
	},
	{
		"des-ecb", &ossl_des_ecb
	},
	{
		"des-cbc", &ossl_des_cbc
	},
	{
		"cast5-ecb", &ossl_cast_ecb
	},
	{
		"cast5-cbc", &ossl_cast_cbc
	},
	{
		NULL
	}
};

/* PUBLIC functions */

int
px_find_cipher(const char *name, PX_Cipher ** res)
{
	unsigned	i;
	PX_Cipher  *c = NULL,
			   *csrc;
	ossldata   *od;
	const struct ossl_cipher *ossl_ciph = NULL;

	name = px_resolve_alias(ossl_aliases, name);
	for (i = 0; ossl_cipher_types[i].name; i++)
	{
		if (!strcmp(ossl_cipher_types[i].name, name))
		{
			ossl_ciph = ossl_cipher_types[i].ciph;
			break;
		}
	}
	if (ossl_ciph == NULL)
		return -1;

	od = px_alloc(sizeof(*od));
	memset(od, 0, sizeof(*od));
	od->ciph = ossl_ciph;

	csrc = NULL;

	c = px_alloc(sizeof(*c));
	c->block_size = gen_ossl_block_size;
	c->key_size = gen_ossl_key_size;
	c->iv_size = gen_ossl_iv_size;
	c->free = gen_ossl_free;
	c->init = ossl_ciph->init;
	c->encrypt = ossl_ciph->encrypt;
	c->decrypt = ossl_ciph->decrypt;
	c->ptr = od;

	*res = c;
	return 0;
}

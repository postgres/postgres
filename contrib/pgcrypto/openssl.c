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
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * contrib/pgcrypto/openssl.c
 */

#include "postgres.h"

#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/rand.h>

#include "px.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

/*
 * Max lengths we might want to handle.
 */
#define MAX_KEY		(512/8)
#define MAX_IV		(128/8)

/*
 * Hashes
 */

/*
 * To make sure we don't leak OpenSSL handles on abort, we keep OSSLDigest
 * objects in a linked list, allocated in TopMemoryContext. We use the
 * ResourceOwner mechanism to free them on abort.
 */
typedef struct OSSLDigest
{
	const EVP_MD *algo;
	EVP_MD_CTX *ctx;

	ResourceOwner owner;
	struct OSSLDigest *next;
	struct OSSLDigest *prev;
} OSSLDigest;

static OSSLDigest *open_digests = NULL;
static bool digest_resowner_callback_registered = false;

static void
free_openssl_digest(OSSLDigest *digest)
{
	EVP_MD_CTX_destroy(digest->ctx);
	if (digest->prev)
		digest->prev->next = digest->next;
	else
		open_digests = digest->next;
	if (digest->next)
		digest->next->prev = digest->prev;
	pfree(digest);
}

/*
 * Close any open OpenSSL handles on abort.
 */
static void
digest_free_callback(ResourceReleasePhase phase,
					 bool isCommit,
					 bool isTopLevel,
					 void *arg)
{
	OSSLDigest *curr;
	OSSLDigest *next;

	if (phase != RESOURCE_RELEASE_AFTER_LOCKS)
		return;

	next = open_digests;
	while (next)
	{
		curr = next;
		next = curr->next;

		if (curr->owner == CurrentResourceOwner)
		{
			if (isCommit)
				elog(WARNING, "pgcrypto digest reference leak: digest %p still referenced", curr);
			free_openssl_digest(curr);
		}
	}
}

static unsigned
digest_result_size(PX_MD *h)
{
	OSSLDigest *digest = (OSSLDigest *) h->p.ptr;
	int			result = EVP_MD_CTX_size(digest->ctx);

	if (result < 0)
		elog(ERROR, "EVP_MD_CTX_size() failed");

	return result;
}

static unsigned
digest_block_size(PX_MD *h)
{
	OSSLDigest *digest = (OSSLDigest *) h->p.ptr;
	int			result = EVP_MD_CTX_block_size(digest->ctx);

	if (result < 0)
		elog(ERROR, "EVP_MD_CTX_block_size() failed");

	return result;
}

static void
digest_reset(PX_MD *h)
{
	OSSLDigest *digest = (OSSLDigest *) h->p.ptr;

	if (!EVP_DigestInit_ex(digest->ctx, digest->algo, NULL))
		elog(ERROR, "EVP_DigestInit_ex() failed");
}

static void
digest_update(PX_MD *h, const uint8 *data, unsigned dlen)
{
	OSSLDigest *digest = (OSSLDigest *) h->p.ptr;

	if (!EVP_DigestUpdate(digest->ctx, data, dlen))
		elog(ERROR, "EVP_DigestUpdate() failed");
}

static void
digest_finish(PX_MD *h, uint8 *dst)
{
	OSSLDigest *digest = (OSSLDigest *) h->p.ptr;

	if (!EVP_DigestFinal_ex(digest->ctx, dst, NULL))
		elog(ERROR, "EVP_DigestFinal_ex() failed");
}

static void
digest_free(PX_MD *h)
{
	OSSLDigest *digest = (OSSLDigest *) h->p.ptr;

	free_openssl_digest(digest);
	pfree(h);
}

static int	px_openssl_initialized = 0;

/* PUBLIC functions */

int
px_find_digest(const char *name, PX_MD **res)
{
	const EVP_MD *md;
	EVP_MD_CTX *ctx;
	PX_MD	   *h;
	OSSLDigest *digest;

	if (!px_openssl_initialized)
	{
		px_openssl_initialized = 1;
		OpenSSL_add_all_algorithms();
	}

	if (!digest_resowner_callback_registered)
	{
		RegisterResourceReleaseCallback(digest_free_callback, NULL);
		digest_resowner_callback_registered = true;
	}

	md = EVP_get_digestbyname(name);
	if (md == NULL)
		return PXE_NO_HASH;

	/*
	 * Create an OSSLDigest object, an OpenSSL MD object, and a PX_MD object.
	 * The order is crucial, to make sure we don't leak anything on
	 * out-of-memory or other error.
	 */
	digest = MemoryContextAlloc(TopMemoryContext, sizeof(*digest));

	ctx = EVP_MD_CTX_create();
	if (!ctx)
	{
		pfree(digest);
		return -1;
	}
	if (EVP_DigestInit_ex(ctx, md, NULL) == 0)
	{
		EVP_MD_CTX_destroy(ctx);
		pfree(digest);
		return -1;
	}

	digest->algo = md;
	digest->ctx = ctx;
	digest->owner = CurrentResourceOwner;
	digest->next = open_digests;
	digest->prev = NULL;
	open_digests = digest;

	/* The PX_MD object is allocated in the current memory context. */
	h = palloc(sizeof(*h));
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
 * We use OpenSSL's EVP* family of functions for these.
 */

/*
 * prototype for the EVP functions that return an algorithm, e.g.
 * EVP_aes_128_cbc().
 */
typedef const EVP_CIPHER *(*ossl_EVP_cipher_func) (void);

/*
 * ossl_cipher contains the static information about each cipher.
 */
struct ossl_cipher
{
	int			(*init) (PX_Cipher *c, const uint8 *key, unsigned klen, const uint8 *iv);
	ossl_EVP_cipher_func cipher_func;
	int			block_size;
	int			max_key_size;
};

/*
 * OSSLCipher contains the state for using a cipher. A separate OSSLCipher
 * object is allocated in each px_find_cipher() call.
 *
 * To make sure we don't leak OpenSSL handles on abort, we keep OSSLCipher
 * objects in a linked list, allocated in TopMemoryContext. We use the
 * ResourceOwner mechanism to free them on abort.
 */
typedef struct OSSLCipher
{
	EVP_CIPHER_CTX *evp_ctx;
	const EVP_CIPHER *evp_ciph;
	uint8		key[MAX_KEY];
	uint8		iv[MAX_IV];
	unsigned	klen;
	unsigned	init;
	const struct ossl_cipher *ciph;

	ResourceOwner owner;
	struct OSSLCipher *next;
	struct OSSLCipher *prev;
} OSSLCipher;

static OSSLCipher *open_ciphers = NULL;
static bool cipher_resowner_callback_registered = false;

static void
free_openssl_cipher(OSSLCipher *od)
{
	EVP_CIPHER_CTX_free(od->evp_ctx);
	if (od->prev)
		od->prev->next = od->next;
	else
		open_ciphers = od->next;
	if (od->next)
		od->next->prev = od->prev;
	pfree(od);
}

/*
 * Close any open OpenSSL cipher handles on abort.
 */
static void
cipher_free_callback(ResourceReleasePhase phase,
					 bool isCommit,
					 bool isTopLevel,
					 void *arg)
{
	OSSLCipher *curr;
	OSSLCipher *next;

	if (phase != RESOURCE_RELEASE_AFTER_LOCKS)
		return;

	next = open_ciphers;
	while (next)
	{
		curr = next;
		next = curr->next;

		if (curr->owner == CurrentResourceOwner)
		{
			if (isCommit)
				elog(WARNING, "pgcrypto cipher reference leak: cipher %p still referenced", curr);
			free_openssl_cipher(curr);
		}
	}
}

/* Common routines for all algorithms */

static unsigned
gen_ossl_block_size(PX_Cipher *c)
{
	OSSLCipher *od = (OSSLCipher *) c->ptr;

	return od->ciph->block_size;
}

static unsigned
gen_ossl_key_size(PX_Cipher *c)
{
	OSSLCipher *od = (OSSLCipher *) c->ptr;

	return od->ciph->max_key_size;
}

static unsigned
gen_ossl_iv_size(PX_Cipher *c)
{
	unsigned	ivlen;
	OSSLCipher *od = (OSSLCipher *) c->ptr;

	ivlen = od->ciph->block_size;
	return ivlen;
}

static void
gen_ossl_free(PX_Cipher *c)
{
	OSSLCipher *od = (OSSLCipher *) c->ptr;

	free_openssl_cipher(od);
	pfree(c);
}

static int
gen_ossl_decrypt(PX_Cipher *c, const uint8 *data, unsigned dlen,
				 uint8 *res)
{
	OSSLCipher *od = c->ptr;
	int			outlen;

	if (!od->init)
	{
		if (!EVP_DecryptInit_ex(od->evp_ctx, od->evp_ciph, NULL, NULL, NULL))
			return PXE_CIPHER_INIT;
		if (!EVP_CIPHER_CTX_set_key_length(od->evp_ctx, od->klen))
			return PXE_CIPHER_INIT;
		if (!EVP_DecryptInit_ex(od->evp_ctx, NULL, NULL, od->key, od->iv))
			return PXE_CIPHER_INIT;
		od->init = true;
	}

	if (!EVP_DecryptUpdate(od->evp_ctx, res, &outlen, data, dlen))
		return PXE_DECRYPT_FAILED;

	return 0;
}

static int
gen_ossl_encrypt(PX_Cipher *c, const uint8 *data, unsigned dlen,
				 uint8 *res)
{
	OSSLCipher *od = c->ptr;
	int			outlen;

	if (!od->init)
	{
		if (!EVP_EncryptInit_ex(od->evp_ctx, od->evp_ciph, NULL, NULL, NULL))
			return PXE_CIPHER_INIT;
		if (!EVP_CIPHER_CTX_set_key_length(od->evp_ctx, od->klen))
			return PXE_CIPHER_INIT;
		if (!EVP_EncryptInit_ex(od->evp_ctx, NULL, NULL, od->key, od->iv))
			return PXE_CIPHER_INIT;
		od->init = true;
	}

	if (!EVP_EncryptUpdate(od->evp_ctx, res, &outlen, data, dlen))
		return PXE_ENCRYPT_FAILED;

	return 0;
}

/* Blowfish */

/*
 * Check if strong crypto is supported. Some OpenSSL installations
 * support only short keys and unfortunately BF_set_key does not return any
 * error value. This function tests if is possible to use strong key.
 */
static int
bf_check_supported_key_len(void)
{
	static const uint8 key[56] = {
		0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87, 0x78, 0x69,
		0x5a, 0x4b, 0x3c, 0x2d, 0x1e, 0x0f, 0x00, 0x11, 0x22, 0x33,
		0x44, 0x55, 0x66, 0x77, 0x04, 0x68, 0x91, 0x04, 0xc2, 0xfd,
		0x3b, 0x2f, 0x58, 0x40, 0x23, 0x64, 0x1a, 0xba, 0x61, 0x76,
		0x1f, 0x1f, 0x1f, 0x1f, 0x0e, 0x0e, 0x0e, 0x0e, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};

	static const uint8 data[8] = {0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};
	static const uint8 res[8] = {0xc0, 0x45, 0x04, 0x01, 0x2e, 0x4e, 0x1f, 0x53};
	uint8		out[8];
	EVP_CIPHER_CTX *evp_ctx;
	int			outlen;
	int			status = 0;

	/* encrypt with 448bits key and verify output */
	evp_ctx = EVP_CIPHER_CTX_new();
	if (!evp_ctx)
		return 0;
	if (!EVP_EncryptInit_ex(evp_ctx, EVP_bf_ecb(), NULL, NULL, NULL))
		goto leave;
	if (!EVP_CIPHER_CTX_set_key_length(evp_ctx, 56))
		goto leave;
	if (!EVP_EncryptInit_ex(evp_ctx, NULL, NULL, key, NULL))
		goto leave;

	if (!EVP_EncryptUpdate(evp_ctx, out, &outlen, data, 8))
		goto leave;

	if (memcmp(out, res, 8) != 0)
		goto leave;				/* Output does not match -> strong cipher is
								 * not supported */
	status = 1;

leave:
	EVP_CIPHER_CTX_free(evp_ctx);
	return status;
}

static int
bf_init(PX_Cipher *c, const uint8 *key, unsigned klen, const uint8 *iv)
{
	OSSLCipher *od = c->ptr;
	unsigned	bs = gen_ossl_block_size(c);
	static int	bf_is_strong = -1;

	/*
	 * Test if key len is supported. BF_set_key silently cut large keys and it
	 * could be a problem when user transfer crypted data from one server to
	 * another.
	 */

	if (bf_is_strong == -1)
		bf_is_strong = bf_check_supported_key_len();

	if (!bf_is_strong && klen > 16)
		return PXE_KEY_TOO_BIG;

	/* Key len is supported. We can use it. */
	od->klen = klen;
	memcpy(od->key, key, klen);

	if (iv)
		memcpy(od->iv, iv, bs);
	else
		memset(od->iv, 0, bs);
	return 0;
}

/* DES */

static int
ossl_des_init(PX_Cipher *c, const uint8 *key, unsigned klen, const uint8 *iv)
{
	OSSLCipher *od = c->ptr;
	unsigned	bs = gen_ossl_block_size(c);

	od->klen = 8;
	memset(od->key, 0, 8);
	memcpy(od->key, key, klen > 8 ? 8 : klen);

	if (iv)
		memcpy(od->iv, iv, bs);
	else
		memset(od->iv, 0, bs);
	return 0;
}

/* DES3 */

static int
ossl_des3_init(PX_Cipher *c, const uint8 *key, unsigned klen, const uint8 *iv)
{
	OSSLCipher *od = c->ptr;
	unsigned	bs = gen_ossl_block_size(c);

	od->klen = 24;
	memset(od->key, 0, 24);
	memcpy(od->key, key, klen > 24 ? 24 : klen);

	if (iv)
		memcpy(od->iv, iv, bs);
	else
		memset(od->iv, 0, bs);
	return 0;
}

/* CAST5 */

static int
ossl_cast_init(PX_Cipher *c, const uint8 *key, unsigned klen, const uint8 *iv)
{
	OSSLCipher *od = c->ptr;
	unsigned	bs = gen_ossl_block_size(c);

	od->klen = klen;
	memcpy(od->key, key, klen);

	if (iv)
		memcpy(od->iv, iv, bs);
	else
		memset(od->iv, 0, bs);
	return 0;
}

/* AES */

static int
ossl_aes_init(PX_Cipher *c, const uint8 *key, unsigned klen, const uint8 *iv)
{
	OSSLCipher *od = c->ptr;
	unsigned	bs = gen_ossl_block_size(c);

	if (klen <= 128 / 8)
		od->klen = 128 / 8;
	else if (klen <= 192 / 8)
		od->klen = 192 / 8;
	else if (klen <= 256 / 8)
		od->klen = 256 / 8;
	else
		return PXE_KEY_TOO_BIG;

	memcpy(od->key, key, klen);

	if (iv)
		memcpy(od->iv, iv, bs);
	else
		memset(od->iv, 0, bs);

	return 0;
}

static int
ossl_aes_ecb_init(PX_Cipher *c, const uint8 *key, unsigned klen, const uint8 *iv)
{
	OSSLCipher *od = c->ptr;
	int			err;

	err = ossl_aes_init(c, key, klen, iv);
	if (err)
		return err;

	switch (od->klen)
	{
		case 128 / 8:
			od->evp_ciph = EVP_aes_128_ecb();
			break;
		case 192 / 8:
			od->evp_ciph = EVP_aes_192_ecb();
			break;
		case 256 / 8:
			od->evp_ciph = EVP_aes_256_ecb();
			break;
		default:
			/* shouldn't happen */
			err = PXE_CIPHER_INIT;
			break;
	}

	return err;
}

static int
ossl_aes_cbc_init(PX_Cipher *c, const uint8 *key, unsigned klen, const uint8 *iv)
{
	OSSLCipher *od = c->ptr;
	int			err;

	err = ossl_aes_init(c, key, klen, iv);
	if (err)
		return err;

	switch (od->klen)
	{
		case 128 / 8:
			od->evp_ciph = EVP_aes_128_cbc();
			break;
		case 192 / 8:
			od->evp_ciph = EVP_aes_192_cbc();
			break;
		case 256 / 8:
			od->evp_ciph = EVP_aes_256_cbc();
			break;
		default:
			/* shouldn't happen */
			err = PXE_CIPHER_INIT;
			break;
	}

	return err;
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
	{"3des", "des3-cbc"},
	{"3des-ecb", "des3-ecb"},
	{"3des-cbc", "des3-cbc"},
	{"cast5", "cast5-cbc"},
	{"aes", "aes-cbc"},
	{"rijndael", "aes-cbc"},
	{"rijndael-cbc", "aes-cbc"},
	{"rijndael-ecb", "aes-ecb"},
	{NULL}
};

static const struct ossl_cipher ossl_bf_cbc = {
	bf_init,
	EVP_bf_cbc,
	64 / 8, 448 / 8
};

static const struct ossl_cipher ossl_bf_ecb = {
	bf_init,
	EVP_bf_ecb,
	64 / 8, 448 / 8
};

static const struct ossl_cipher ossl_bf_cfb = {
	bf_init,
	EVP_bf_cfb,
	64 / 8, 448 / 8
};

static const struct ossl_cipher ossl_des_ecb = {
	ossl_des_init,
	EVP_des_ecb,
	64 / 8, 64 / 8
};

static const struct ossl_cipher ossl_des_cbc = {
	ossl_des_init,
	EVP_des_cbc,
	64 / 8, 64 / 8
};

static const struct ossl_cipher ossl_des3_ecb = {
	ossl_des3_init,
	EVP_des_ede3_ecb,
	64 / 8, 192 / 8
};

static const struct ossl_cipher ossl_des3_cbc = {
	ossl_des3_init,
	EVP_des_ede3_cbc,
	64 / 8, 192 / 8
};

static const struct ossl_cipher ossl_cast_ecb = {
	ossl_cast_init,
	EVP_cast5_ecb,
	64 / 8, 128 / 8
};

static const struct ossl_cipher ossl_cast_cbc = {
	ossl_cast_init,
	EVP_cast5_cbc,
	64 / 8, 128 / 8
};

static const struct ossl_cipher ossl_aes_ecb = {
	ossl_aes_ecb_init,
	NULL,						/* EVP_aes_XXX_ecb(), determined in init
								 * function */
	128 / 8, 256 / 8
};

static const struct ossl_cipher ossl_aes_cbc = {
	ossl_aes_cbc_init,
	NULL,						/* EVP_aes_XXX_cbc(), determined in init
								 * function */
	128 / 8, 256 / 8
};

/*
 * Special handlers
 */
struct ossl_cipher_lookup
{
	const char *name;
	const struct ossl_cipher *ciph;
};

static const struct ossl_cipher_lookup ossl_cipher_types[] = {
	{"bf-cbc", &ossl_bf_cbc},
	{"bf-ecb", &ossl_bf_ecb},
	{"bf-cfb", &ossl_bf_cfb},
	{"des-ecb", &ossl_des_ecb},
	{"des-cbc", &ossl_des_cbc},
	{"des3-ecb", &ossl_des3_ecb},
	{"des3-cbc", &ossl_des3_cbc},
	{"cast5-ecb", &ossl_cast_ecb},
	{"cast5-cbc", &ossl_cast_cbc},
	{"aes-ecb", &ossl_aes_ecb},
	{"aes-cbc", &ossl_aes_cbc},
	{NULL}
};

/* PUBLIC functions */

int
px_find_cipher(const char *name, PX_Cipher **res)
{
	const struct ossl_cipher_lookup *i;
	PX_Cipher  *c = NULL;
	EVP_CIPHER_CTX *ctx;
	OSSLCipher *od;

	name = px_resolve_alias(ossl_aliases, name);
	for (i = ossl_cipher_types; i->name; i++)
		if (strcmp(i->name, name) == 0)
			break;
	if (i->name == NULL)
		return PXE_NO_CIPHER;

	if (!cipher_resowner_callback_registered)
	{
		RegisterResourceReleaseCallback(cipher_free_callback, NULL);
		cipher_resowner_callback_registered = true;
	}

	/*
	 * Create an OSSLCipher object, an EVP_CIPHER_CTX object and a PX_Cipher.
	 * The order is crucial, to make sure we don't leak anything on
	 * out-of-memory or other error.
	 */
	od = MemoryContextAllocZero(TopMemoryContext, sizeof(*od));
	od->ciph = i->ciph;

	/* Allocate an EVP_CIPHER_CTX object. */
	ctx = EVP_CIPHER_CTX_new();
	if (!ctx)
	{
		pfree(od);
		return PXE_CIPHER_INIT;
	}

	od->evp_ctx = ctx;
	od->owner = CurrentResourceOwner;
	od->next = open_ciphers;
	od->prev = NULL;
	open_ciphers = od;

	if (i->ciph->cipher_func)
		od->evp_ciph = i->ciph->cipher_func();

	/* The PX_Cipher is allocated in current memory context */
	c = palloc(sizeof(*c));
	c->block_size = gen_ossl_block_size;
	c->key_size = gen_ossl_key_size;
	c->iv_size = gen_ossl_iv_size;
	c->free = gen_ossl_free;
	c->init = od->ciph->init;
	c->encrypt = gen_ossl_encrypt;
	c->decrypt = gen_ossl_decrypt;
	c->ptr = od;

	*res = c;
	return 0;
}

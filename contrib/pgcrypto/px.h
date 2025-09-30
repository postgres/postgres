/*
 * px.h
 *		Header file for pgcrypto.
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
 * contrib/pgcrypto/px.h
 */

#ifndef __PX_H
#define __PX_H

#include <sys/param.h>

/* keep debug messages? */
#define PX_DEBUG

/* max salt returned */
#define PX_MAX_SALT_LEN		128

/*
 * PX error codes
 */
#define PXE_OK						0
/* -1 is unused */
#define PXE_NO_HASH					-2
#define PXE_NO_CIPHER				-3
/* -4 is unused */
#define PXE_BAD_OPTION				-5
#define PXE_BAD_FORMAT				-6
#define PXE_KEY_TOO_BIG				-7
#define PXE_CIPHER_INIT				-8
#define PXE_HASH_UNUSABLE_FOR_HMAC	-9
/* -10 is unused */
/* -11 is unused */
#define PXE_BUG						-12
#define PXE_ARGUMENT_ERROR			-13
#define PXE_UNKNOWN_SALT_ALGO		-14
#define PXE_BAD_SALT_ROUNDS			-15
/* -16 is unused */
#define PXE_NO_RANDOM				-17
#define PXE_DECRYPT_FAILED			-18
#define PXE_ENCRYPT_FAILED			-19

#define PXE_PGP_CORRUPT_DATA		-100
#define PXE_PGP_CORRUPT_ARMOR		-101
#define PXE_PGP_UNSUPPORTED_COMPR	-102
#define PXE_PGP_UNSUPPORTED_CIPHER	-103
#define PXE_PGP_UNSUPPORTED_HASH	-104
#define PXE_PGP_COMPRESSION_ERROR	-105
#define PXE_PGP_NOT_TEXT			-106
#define PXE_PGP_UNEXPECTED_PKT		-107
/* -108 is unused */
#define PXE_PGP_MATH_FAILED			-109
#define PXE_PGP_SHORT_ELGAMAL_KEY	-110
/* -111 is unused */
#define PXE_PGP_UNKNOWN_PUBALGO		-112
#define PXE_PGP_WRONG_KEY			-113
#define PXE_PGP_MULTIPLE_KEYS		-114
#define PXE_PGP_EXPECT_PUBLIC_KEY	-115
#define PXE_PGP_EXPECT_SECRET_KEY	-116
#define PXE_PGP_NOT_V4_KEYPKT		-117
#define PXE_PGP_KEYPKT_CORRUPT		-118
#define PXE_PGP_NO_USABLE_KEY		-119
#define PXE_PGP_NEED_SECRET_PSW		-120
#define PXE_PGP_BAD_S2K_MODE		-121
#define PXE_PGP_UNSUPPORTED_PUBALGO -122
#define PXE_PGP_MULTIPLE_SUBKEYS	-123

typedef enum BuiltinCryptoOptions
{
	BC_ON,
	BC_OFF,
	BC_FIPS,
}			BuiltinCryptoOptions;

typedef struct px_digest PX_MD;
typedef struct px_alias PX_Alias;
typedef struct px_hmac PX_HMAC;
typedef struct px_cipher PX_Cipher;
typedef struct px_combo PX_Combo;

extern int	builtin_crypto_enabled;

struct px_digest
{
	unsigned	(*result_size) (PX_MD *h);
	unsigned	(*block_size) (PX_MD *h);
	void		(*reset) (PX_MD *h);
	void		(*update) (PX_MD *h, const uint8 *data, unsigned dlen);
	void		(*finish) (PX_MD *h, uint8 *dst);
	void		(*free) (PX_MD *h);
	/* private */
	union
	{
		unsigned	code;
		void	   *ptr;
	}			p;
};

struct px_alias
{
	char	   *alias;
	char	   *name;
};

struct px_hmac
{
	unsigned	(*result_size) (PX_HMAC *h);
	unsigned	(*block_size) (PX_HMAC *h);
	void		(*reset) (PX_HMAC *h);
	void		(*update) (PX_HMAC *h, const uint8 *data, unsigned dlen);
	void		(*finish) (PX_HMAC *h, uint8 *dst);
	void		(*free) (PX_HMAC *h);
	void		(*init) (PX_HMAC *h, const uint8 *key, unsigned klen);

	PX_MD	   *md;
	/* private */
	struct
	{
		uint8	   *ipad;
		uint8	   *opad;
	}			p;
};

struct px_cipher
{
	unsigned	(*block_size) (PX_Cipher *c);
	unsigned	(*key_size) (PX_Cipher *c); /* max key len */
	unsigned	(*iv_size) (PX_Cipher *c);

	int			(*init) (PX_Cipher *c, const uint8 *key, unsigned klen, const uint8 *iv);
	int			(*encrypt) (PX_Cipher *c, int padding, const uint8 *data, unsigned dlen, uint8 *res, unsigned *rlen);
	int			(*decrypt) (PX_Cipher *c, int padding, const uint8 *data, unsigned dlen, uint8 *res, unsigned *rlen);
	void		(*free) (PX_Cipher *c);
	/* private */
	void	   *ptr;
	int			pstat;			/* mcrypt uses it */
};

struct px_combo
{
	int			(*init) (PX_Combo *cx, const uint8 *key, unsigned klen,
						 const uint8 *iv, unsigned ivlen);
	int			(*encrypt) (PX_Combo *cx, const uint8 *data, unsigned dlen,
							uint8 *res, unsigned *rlen);
	int			(*decrypt) (PX_Combo *cx, const uint8 *data, unsigned dlen,
							uint8 *res, unsigned *rlen);
	unsigned	(*encrypt_len) (PX_Combo *cx, unsigned dlen);
	unsigned	(*decrypt_len) (PX_Combo *cx, unsigned dlen);
	void		(*free) (PX_Combo *cx);

	PX_Cipher  *cipher;
	unsigned	padding;
};

int			px_find_digest(const char *name, PX_MD **res);
int			px_find_hmac(const char *name, PX_HMAC **res);
int			px_find_cipher(const char *name, PX_Cipher **res);
int			px_find_combo(const char *name, PX_Combo **res);

pg_noreturn void px_THROW_ERROR(int err);
const char *px_strerror(int err);

const char *px_resolve_alias(const PX_Alias *list, const char *name);

void		px_set_debug_handler(void (*handler) (const char *));

void		px_memset(void *ptr, int c, size_t len);

bool		CheckFIPSMode(void);
void		CheckBuiltinCryptoMode(void);

#ifdef PX_DEBUG
void		px_debug(const char *fmt,...) pg_attribute_printf(1, 2);
#else
#define px_debug(...)
#endif

#define px_md_result_size(md)		(md)->result_size(md)
#define px_md_block_size(md)		(md)->block_size(md)
#define px_md_reset(md)			(md)->reset(md)
#define px_md_update(md, data, dlen)	(md)->update(md, data, dlen)
#define px_md_finish(md, buf)		(md)->finish(md, buf)
#define px_md_free(md)			(md)->free(md)

#define px_hmac_result_size(hmac)	(hmac)->result_size(hmac)
#define px_hmac_block_size(hmac)	(hmac)->block_size(hmac)
#define px_hmac_reset(hmac)		(hmac)->reset(hmac)
#define px_hmac_init(hmac, key, klen)	(hmac)->init(hmac, key, klen)
#define px_hmac_update(hmac, data, dlen) (hmac)->update(hmac, data, dlen)
#define px_hmac_finish(hmac, buf)	(hmac)->finish(hmac, buf)
#define px_hmac_free(hmac)		(hmac)->free(hmac)


#define px_cipher_key_size(c)		(c)->key_size(c)
#define px_cipher_block_size(c)		(c)->block_size(c)
#define px_cipher_iv_size(c)		(c)->iv_size(c)
#define px_cipher_init(c, k, klen, iv)	(c)->init(c, k, klen, iv)
#define px_cipher_encrypt(c, padding, data, dlen, res, rlen) \
					(c)->encrypt(c, padding, data, dlen, res, rlen)
#define px_cipher_decrypt(c, padding, data, dlen, res, rlen) \
					(c)->decrypt(c, padding, data, dlen, res, rlen)
#define px_cipher_free(c)		(c)->free(c)


#define px_combo_encrypt_len(c, dlen)	(c)->encrypt_len(c, dlen)
#define px_combo_decrypt_len(c, dlen)	(c)->decrypt_len(c, dlen)
#define px_combo_init(c, key, klen, iv, ivlen) \
					(c)->init(c, key, klen, iv, ivlen)
#define px_combo_encrypt(c, data, dlen, res, rlen) \
					(c)->encrypt(c, data, dlen, res, rlen)
#define px_combo_decrypt(c, data, dlen, res, rlen) \
					(c)->decrypt(c, data, dlen, res, rlen)
#define px_combo_free(c)		(c)->free(c)

#endif							/* __PX_H */

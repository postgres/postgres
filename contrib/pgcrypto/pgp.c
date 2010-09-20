/*
 * pgp.c
 *	  Various utility stuff.
 *
 * Copyright (c) 2005 Marko Kreen
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
 * contrib/pgcrypto/pgp.c
 */

#include "postgres.h"

#include "px.h"
#include "mbuf.h"
#include "pgp.h"

/*
 * Defaults.
 */
static int	def_cipher_algo = PGP_SYM_AES_128;
static int	def_s2k_cipher_algo = -1;
static int	def_s2k_mode = PGP_S2K_ISALTED;
static int	def_s2k_digest_algo = PGP_DIGEST_SHA1;
static int	def_compress_algo = PGP_COMPR_NONE;
static int	def_compress_level = 6;
static int	def_disable_mdc = 0;
static int	def_use_sess_key = 0;
static int	def_text_mode = 0;
static int	def_unicode_mode = 0;
static int	def_convert_crlf = 0;

struct digest_info
{
	const char *name;
	int			code;
	const char *int_name;
};

struct cipher_info
{
	const char *name;
	int			code;
	const char *int_name;
	int			key_len;
	int			block_len;
};

static const struct digest_info digest_list[] = {
	{"md5", PGP_DIGEST_MD5},
	{"sha1", PGP_DIGEST_SHA1},
	{"sha-1", PGP_DIGEST_SHA1},
	{"ripemd160", PGP_DIGEST_RIPEMD160},
	{"sha256", PGP_DIGEST_SHA256},
	{"sha384", PGP_DIGEST_SHA384},
	{"sha512", PGP_DIGEST_SHA512},
	{NULL, 0}
};

static const struct cipher_info cipher_list[] = {
	{"3des", PGP_SYM_DES3, "3des-ecb", 192 / 8, 64 / 8},
	{"cast5", PGP_SYM_CAST5, "cast5-ecb", 128 / 8, 64 / 8},
	{"bf", PGP_SYM_BLOWFISH, "bf-ecb", 128 / 8, 64 / 8},
	{"blowfish", PGP_SYM_BLOWFISH, "bf-ecb", 128 / 8, 64 / 8},
	{"aes", PGP_SYM_AES_128, "aes-ecb", 128 / 8, 128 / 8},
	{"aes128", PGP_SYM_AES_128, "aes-ecb", 128 / 8, 128 / 8},
	{"aes192", PGP_SYM_AES_192, "aes-ecb", 192 / 8, 128 / 8},
	{"aes256", PGP_SYM_AES_256, "aes-ecb", 256 / 8, 128 / 8},
	{"twofish", PGP_SYM_TWOFISH, "twofish-ecb", 256 / 8, 128 / 8},
	{NULL, 0, NULL}
};

static const struct cipher_info *
get_cipher_info(int code)
{
	const struct cipher_info *i;

	for (i = cipher_list; i->name; i++)
		if (i->code == code)
			return i;
	return NULL;
}

int
pgp_get_digest_code(const char *name)
{
	const struct digest_info *i;

	for (i = digest_list; i->name; i++)
		if (pg_strcasecmp(i->name, name) == 0)
			return i->code;
	return PXE_PGP_UNSUPPORTED_HASH;
}

int
pgp_get_cipher_code(const char *name)
{
	const struct cipher_info *i;

	for (i = cipher_list; i->name; i++)
		if (pg_strcasecmp(i->name, name) == 0)
			return i->code;
	return PXE_PGP_UNSUPPORTED_CIPHER;
}

const char *
pgp_get_digest_name(int code)
{
	const struct digest_info *i;

	for (i = digest_list; i->name; i++)
		if (i->code == code)
			return i->name;
	return NULL;
}

const char *
pgp_get_cipher_name(int code)
{
	const struct cipher_info *i = get_cipher_info(code);

	if (i != NULL)
		return i->name;
	return NULL;
}

int
pgp_get_cipher_key_size(int code)
{
	const struct cipher_info *i = get_cipher_info(code);

	if (i != NULL)
		return i->key_len;
	return 0;
}

int
pgp_get_cipher_block_size(int code)
{
	const struct cipher_info *i = get_cipher_info(code);

	if (i != NULL)
		return i->block_len;
	return 0;
}

int
pgp_load_cipher(int code, PX_Cipher **res)
{
	int			err;
	const struct cipher_info *i = get_cipher_info(code);

	if (i == NULL)
		return PXE_PGP_CORRUPT_DATA;

	err = px_find_cipher(i->int_name, res);
	if (err == 0)
		return 0;

	return PXE_PGP_UNSUPPORTED_CIPHER;
}

int
pgp_load_digest(int code, PX_MD **res)
{
	int			err;
	const char *name = pgp_get_digest_name(code);

	if (name == NULL)
		return PXE_PGP_CORRUPT_DATA;

	err = px_find_digest(name, res);
	if (err == 0)
		return 0;

	return PXE_PGP_UNSUPPORTED_HASH;
}

int
pgp_init(PGP_Context **ctx_p)
{
	PGP_Context *ctx;

	ctx = px_alloc(sizeof *ctx);
	memset(ctx, 0, sizeof *ctx);

	ctx->cipher_algo = def_cipher_algo;
	ctx->s2k_cipher_algo = def_s2k_cipher_algo;
	ctx->s2k_mode = def_s2k_mode;
	ctx->s2k_digest_algo = def_s2k_digest_algo;
	ctx->compress_algo = def_compress_algo;
	ctx->compress_level = def_compress_level;
	ctx->disable_mdc = def_disable_mdc;
	ctx->use_sess_key = def_use_sess_key;
	ctx->unicode_mode = def_unicode_mode;
	ctx->convert_crlf = def_convert_crlf;
	ctx->text_mode = def_text_mode;

	*ctx_p = ctx;
	return 0;
}

int
pgp_free(PGP_Context *ctx)
{
	if (ctx->pub_key)
		pgp_key_free(ctx->pub_key);
	memset(ctx, 0, sizeof *ctx);
	px_free(ctx);
	return 0;
}

int
pgp_disable_mdc(PGP_Context *ctx, int disable)
{
	ctx->disable_mdc = disable ? 1 : 0;
	return 0;
}

int
pgp_set_sess_key(PGP_Context *ctx, int use)
{
	ctx->use_sess_key = use ? 1 : 0;
	return 0;
}

int
pgp_set_convert_crlf(PGP_Context *ctx, int doit)
{
	ctx->convert_crlf = doit ? 1 : 0;
	return 0;
}

int
pgp_set_s2k_mode(PGP_Context *ctx, int mode)
{
	int			err = PXE_OK;

	switch (mode)
	{
		case PGP_S2K_SIMPLE:
		case PGP_S2K_SALTED:
		case PGP_S2K_ISALTED:
			ctx->s2k_mode = mode;
			break;
		default:
			err = PXE_ARGUMENT_ERROR;
			break;
	}
	return err;
}

int
pgp_set_compress_algo(PGP_Context *ctx, int algo)
{
	switch (algo)
	{
		case PGP_COMPR_NONE:
		case PGP_COMPR_ZIP:
		case PGP_COMPR_ZLIB:
		case PGP_COMPR_BZIP2:
			ctx->compress_algo = algo;
			return 0;
	}
	return PXE_ARGUMENT_ERROR;
}

int
pgp_set_compress_level(PGP_Context *ctx, int level)
{
	if (level >= 0 && level <= 9)
	{
		ctx->compress_level = level;
		return 0;
	}
	return PXE_ARGUMENT_ERROR;
}

int
pgp_set_text_mode(PGP_Context *ctx, int mode)
{
	ctx->text_mode = mode;
	return 0;
}

int
pgp_set_cipher_algo(PGP_Context *ctx, const char *name)
{
	int			code = pgp_get_cipher_code(name);

	if (code < 0)
		return code;
	ctx->cipher_algo = code;
	return 0;
}

int
pgp_set_s2k_cipher_algo(PGP_Context *ctx, const char *name)
{
	int			code = pgp_get_cipher_code(name);

	if (code < 0)
		return code;
	ctx->s2k_cipher_algo = code;
	return 0;
}

int
pgp_set_s2k_digest_algo(PGP_Context *ctx, const char *name)
{
	int			code = pgp_get_digest_code(name);

	if (code < 0)
		return code;
	ctx->s2k_digest_algo = code;
	return 0;
}

int
pgp_get_unicode_mode(PGP_Context *ctx)
{
	return ctx->unicode_mode;
}

int
pgp_set_unicode_mode(PGP_Context *ctx, int mode)
{
	ctx->unicode_mode = mode ? 1 : 0;
	return 0;
}

int
pgp_set_symkey(PGP_Context *ctx, const uint8 *key, int len)
{
	if (key == NULL || len < 1)
		return PXE_ARGUMENT_ERROR;
	ctx->sym_key = key;
	ctx->sym_key_len = len;
	return 0;
}

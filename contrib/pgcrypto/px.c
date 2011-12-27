/*
 * px.c
 *		Various cryptographic stuff for PostgreSQL.
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
 * contrib/pgcrypto/px.c
 */

#include "postgres.h"

#include "px.h"

struct error_desc
{
	int			err;
	const char *desc;
};

static const struct error_desc px_err_list[] = {
	{PXE_OK, "Everything ok"},
	{PXE_ERR_GENERIC, "Some PX error (not specified)"},
	{PXE_NO_HASH, "No such hash algorithm"},
	{PXE_NO_CIPHER, "No such cipher algorithm"},
	{PXE_NOTBLOCKSIZE, "Data not a multiple of block size"},
	{PXE_BAD_OPTION, "Unknown option"},
	{PXE_BAD_FORMAT, "Badly formatted type"},
	{PXE_KEY_TOO_BIG, "Key was too big"},
	{PXE_CIPHER_INIT, "Cipher cannot be initalized ?"},
	{PXE_HASH_UNUSABLE_FOR_HMAC, "This hash algorithm is unusable for HMAC"},
	{PXE_DEV_READ_ERROR, "Error reading from random device"},
	{PXE_OSSL_RAND_ERROR, "OpenSSL PRNG error"},
	{PXE_BUG, "pgcrypto bug"},
	{PXE_ARGUMENT_ERROR, "Illegal argument to function"},
	{PXE_UNKNOWN_SALT_ALGO, "Unknown salt algorithm"},
	{PXE_BAD_SALT_ROUNDS, "Incorrect number of rounds"},
	{PXE_MCRYPT_INTERNAL, "mcrypt internal error"},
	{PXE_NO_RANDOM, "No strong random source"},
	{PXE_DECRYPT_FAILED, "Decryption failed"},
	{PXE_PGP_CORRUPT_DATA, "Wrong key or corrupt data"},
	{PXE_PGP_CORRUPT_ARMOR, "Corrupt ascii-armor"},
	{PXE_PGP_UNSUPPORTED_COMPR, "Unsupported compression algorithm"},
	{PXE_PGP_UNSUPPORTED_CIPHER, "Unsupported cipher algorithm"},
	{PXE_PGP_UNSUPPORTED_HASH, "Unsupported digest algorithm"},
	{PXE_PGP_COMPRESSION_ERROR, "Compression error"},
	{PXE_PGP_NOT_TEXT, "Not text data"},
	{PXE_PGP_UNEXPECTED_PKT, "Unexpected packet in key data"},
	{PXE_PGP_NO_BIGNUM,
		"public-key functions disabled - "
	"pgcrypto needs OpenSSL for bignums"},
	{PXE_PGP_MATH_FAILED, "Math operation failed"},
	{PXE_PGP_SHORT_ELGAMAL_KEY, "Elgamal keys must be at least 1024 bits long"},
	{PXE_PGP_RSA_UNSUPPORTED, "pgcrypto does not support RSA keys"},
	{PXE_PGP_UNKNOWN_PUBALGO, "Unknown public-key encryption algorithm"},
	{PXE_PGP_WRONG_KEY, "Wrong key"},
	{PXE_PGP_MULTIPLE_KEYS,
	"Several keys given - pgcrypto does not handle keyring"},
	{PXE_PGP_EXPECT_PUBLIC_KEY, "Refusing to encrypt with secret key"},
	{PXE_PGP_EXPECT_SECRET_KEY, "Cannot decrypt with public key"},
	{PXE_PGP_NOT_V4_KEYPKT, "Only V4 key packets are supported"},
	{PXE_PGP_KEYPKT_CORRUPT, "Corrupt key packet"},
	{PXE_PGP_NO_USABLE_KEY, "No encryption key found"},
	{PXE_PGP_NEED_SECRET_PSW, "Need password for secret key"},
	{PXE_PGP_BAD_S2K_MODE, "Bad S2K mode"},
	{PXE_PGP_UNSUPPORTED_PUBALGO, "Unsupported public key algorithm"},
	{PXE_PGP_MULTIPLE_SUBKEYS, "Several subkeys not supported"},

	/* fake this as PXE_PGP_CORRUPT_DATA */
	{PXE_MBUF_SHORT_READ, "Corrupt data"},

	{0, NULL},
};

const char *
px_strerror(int err)
{
	const struct error_desc *e;

	for (e = px_err_list; e->desc; e++)
		if (e->err == err)
			return e->desc;
	return "Bad error code";
}


const char *
px_resolve_alias(const PX_Alias *list, const char *name)
{
	while (list->name)
	{
		if (pg_strcasecmp(list->alias, name) == 0)
			return list->name;
		list++;
	}
	return name;
}

static void (*debug_handler) (const char *) = NULL;

void
px_set_debug_handler(void (*handler) (const char *))
{
	debug_handler = handler;
}

void
px_debug(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	if (debug_handler)
	{
		char		buf[512];

		vsnprintf(buf, sizeof(buf), fmt, ap);
		debug_handler(buf);
	}
	va_end(ap);
}

/*
 * combo - cipher + padding (+ checksum)
 */

static unsigned
combo_encrypt_len(PX_Combo *cx, unsigned dlen)
{
	return dlen + 512;
}

static unsigned
combo_decrypt_len(PX_Combo *cx, unsigned dlen)
{
	return dlen;
}

static int
combo_init(PX_Combo *cx, const uint8 *key, unsigned klen,
		   const uint8 *iv, unsigned ivlen)
{
	int			err;
	unsigned	ks,
				ivs;
	PX_Cipher  *c = cx->cipher;
	uint8	   *ivbuf = NULL;
	uint8	   *keybuf;

	ks = px_cipher_key_size(c);

	ivs = px_cipher_iv_size(c);
	if (ivs > 0)
	{
		ivbuf = px_alloc(ivs);
		memset(ivbuf, 0, ivs);
		if (ivlen > ivs)
			memcpy(ivbuf, iv, ivs);
		else
			memcpy(ivbuf, iv, ivlen);
	}

	if (klen > ks)
		klen = ks;
	keybuf = px_alloc(ks);
	memset(keybuf, 0, ks);
	memcpy(keybuf, key, klen);

	err = px_cipher_init(c, keybuf, klen, ivbuf);

	if (ivbuf)
		px_free(ivbuf);
	px_free(keybuf);

	return err;
}

static int
combo_encrypt(PX_Combo *cx, const uint8 *data, unsigned dlen,
			  uint8 *res, unsigned *rlen)
{
	int			err = 0;
	uint8	   *bbuf;
	unsigned	bs,
				bpos,
				i,
				pad;

	PX_Cipher  *c = cx->cipher;

	bbuf = NULL;
	bs = px_cipher_block_size(c);

	/* encrypt */
	if (bs > 1)
	{
		bbuf = px_alloc(bs * 4);
		bpos = dlen % bs;
		*rlen = dlen - bpos;
		memcpy(bbuf, data + *rlen, bpos);

		/* encrypt full-block data */
		if (*rlen)
		{
			err = px_cipher_encrypt(c, data, *rlen, res);
			if (err)
				goto out;
		}

		/* bbuf has now bpos bytes of stuff */
		if (cx->padding)
		{
			pad = bs - (bpos % bs);
			for (i = 0; i < pad; i++)
				bbuf[bpos++] = pad;
		}
		else if (bpos % bs)
		{
			/* ERROR? */
			pad = bs - (bpos % bs);
			for (i = 0; i < pad; i++)
				bbuf[bpos++] = 0;
		}

		/* encrypt the rest - pad */
		if (bpos)
		{
			err = px_cipher_encrypt(c, bbuf, bpos, res + *rlen);
			*rlen += bpos;
		}
	}
	else
	{
		/* stream cipher/mode - no pad needed */
		err = px_cipher_encrypt(c, data, dlen, res);
		if (err)
			goto out;
		*rlen = dlen;
	}
out:
	if (bbuf)
		px_free(bbuf);

	return err;
}

static int
combo_decrypt(PX_Combo *cx, const uint8 *data, unsigned dlen,
			  uint8 *res, unsigned *rlen)
{
	unsigned	bs,
				i,
				pad;
	unsigned	pad_ok;

	PX_Cipher  *c = cx->cipher;

	/* decide whether zero-length input is allowed */
	if (dlen == 0)
	{
		/* with padding, empty ciphertext is not allowed */
		if (cx->padding)
			return PXE_DECRYPT_FAILED;

		/* without padding, report empty result */
		*rlen = 0;
		return 0;
	}

	bs = px_cipher_block_size(c);
	if (bs > 1 && (dlen % bs) != 0)
		goto block_error;

	/* decrypt */
	*rlen = dlen;
	px_cipher_decrypt(c, data, dlen, res);

	/* unpad */
	if (bs > 1 && cx->padding)
	{
		pad = res[*rlen - 1];
		pad_ok = 0;
		if (pad > 0 && pad <= bs && pad <= *rlen)
		{
			pad_ok = 1;
			for (i = *rlen - pad; i < *rlen; i++)
				if (res[i] != pad)
				{
					pad_ok = 0;
					break;
				}
		}

		if (pad_ok)
			*rlen -= pad;
	}

	return 0;

block_error:
	return PXE_NOTBLOCKSIZE;
}

static void
combo_free(PX_Combo *cx)
{
	if (cx->cipher)
		px_cipher_free(cx->cipher);
	memset(cx, 0, sizeof(*cx));
	px_free(cx);
}

/* PARSER */

static int
parse_cipher_name(char *full, char **cipher, char **pad)
{
	char	   *p,
			   *p2,
			   *q;

	*cipher = full;
	*pad = NULL;

	p = strchr(full, '/');
	if (p != NULL)
		*p++ = 0;
	while (p != NULL)
	{
		if ((q = strchr(p, '/')) != NULL)
			*q++ = 0;

		if (!*p)
		{
			p = q;
			continue;
		}
		p2 = strchr(p, ':');
		if (p2 != NULL)
		{
			*p2++ = 0;
			if (strcmp(p, "pad") == 0)
				*pad = p2;
			else
				return PXE_BAD_OPTION;
		}
		else
			return PXE_BAD_FORMAT;

		p = q;
	}
	return 0;
}

/* provider */

int
px_find_combo(const char *name, PX_Combo **res)
{
	int			err;
	char	   *buf,
			   *s_cipher,
			   *s_pad;

	PX_Combo   *cx;

	cx = px_alloc(sizeof(*cx));
	memset(cx, 0, sizeof(*cx));

	buf = px_alloc(strlen(name) + 1);
	strcpy(buf, name);

	err = parse_cipher_name(buf, &s_cipher, &s_pad);
	if (err)
	{
		px_free(buf);
		px_free(cx);
		return err;
	}

	err = px_find_cipher(s_cipher, &cx->cipher);
	if (err)
		goto err1;

	if (s_pad != NULL)
	{
		if (strcmp(s_pad, "pkcs") == 0)
			cx->padding = 1;
		else if (strcmp(s_pad, "none") == 0)
			cx->padding = 0;
		else
			goto err1;
	}
	else
		cx->padding = 1;

	cx->init = combo_init;
	cx->encrypt = combo_encrypt;
	cx->decrypt = combo_decrypt;
	cx->encrypt_len = combo_encrypt_len;
	cx->decrypt_len = combo_decrypt_len;
	cx->free = combo_free;

	px_free(buf);

	*res = cx;

	return 0;

err1:
	if (cx->cipher)
		px_cipher_free(cx->cipher);
	px_free(cx);
	px_free(buf);
	return PXE_NO_CIPHER;
}

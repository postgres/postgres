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
 * $Id: px.c,v 1.7 2002/03/06 06:09:10 momjian Exp $
 */

#include <postgres.h>

#include "px.h"


const char *
px_resolve_alias(const PX_Alias * list, const char *name)
{
	while (list->name)
	{
		if (!strcasecmp(list->alias, name))
			return list->name;
		list++;
	}
	return name;
}

/*
 * combo - cipher + padding (+ checksum)
 */

static unsigned
combo_encrypt_len(PX_Combo * cx, unsigned dlen)
{
	return dlen + 512;
}

static unsigned
combo_decrypt_len(PX_Combo * cx, unsigned dlen)
{
	return dlen;
}

static int
combo_init(PX_Combo * cx, const uint8 *key, unsigned klen,
		   const uint8 *iv, unsigned ivlen)
{
	int			err;
	unsigned	bs,
				ks,
				ivs;
	PX_Cipher  *c = cx->cipher;
	uint8	   *ivbuf = NULL;
	uint8	   *keybuf;

	bs = px_cipher_block_size(c);
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
combo_encrypt(PX_Combo * cx, const uint8 *data, unsigned dlen,
			  uint8 *res, unsigned *rlen)
{
	int			err = 0;
	uint8	   *bbuf;
	unsigned	bs,
				maxlen,
				bpos,
				i,
				pad;

	PX_Cipher  *c = cx->cipher;

	bbuf = NULL;
	maxlen = *rlen;
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
combo_decrypt(PX_Combo * cx, const uint8 *data, unsigned dlen,
			  uint8 *res, unsigned *rlen)
{
	unsigned	bs,
				i,
				pad;
	unsigned	pad_ok;

	PX_Cipher  *c = cx->cipher;

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

	/* error reporting should be done in pgcrypto.c */
block_error:
	elog(WARNING, "Data not a multiple of block size");
	return -1;
}

static void
combo_free(PX_Combo * cx)
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
			if (!strcmp(p, "pad"))
				*pad = p2;
			else
				return -1;
		}
		else
			return -1;

		p = q;
	}
	return 0;
}

/* provider */

int
px_find_combo(const char *name, PX_Combo ** res)
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
		if (!strcmp(s_pad, "pkcs"))
			cx->padding = 1;
		else if (!strcmp(s_pad, "none"))
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
	return -1;
}

/*
 * encode.c
 *		Various data encoding/decoding things.
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
 * $Id: encode.c,v 1.4 2001/03/22 03:59:10 momjian Exp $
 */

#include "postgres.h"

#include "fmgr.h"

#include "encode.h"

/*
 * NAMEDATALEN is used for hash names
 */
#if NAMEDATALEN < 16
#error "NAMEDATALEN < 16: too small"
#endif

static pg_coding *
			find_coding(pg_coding * hbuf, text *name, int silent);
static pg_coding *
			pg_find_coding(pg_coding * res, char *name);


/* SQL function: encode(bytea, text) returns text */
PG_FUNCTION_INFO_V1(encode);

Datum
encode(PG_FUNCTION_ARGS)
{
	text	   *arg;
	text	   *name;
	uint		len,
				rlen,
				rlen0;
	pg_coding  *c,
				cbuf;
	text	   *res;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();

	name = PG_GETARG_TEXT_P(1);
	c = find_coding(&cbuf, name, 0);	/* will give error if fails */

	arg = PG_GETARG_TEXT_P(0);
	len = VARSIZE(arg) - VARHDRSZ;

	rlen0 = c->encode_len(len);

	res = (text *) palloc(rlen0 + VARHDRSZ);

	rlen = c->encode(VARDATA(arg), len, VARDATA(res));
	VARATT_SIZEP(res) = rlen + VARHDRSZ;

	if (rlen > rlen0)
		elog(FATAL, "pg_encode: overflow, encode estimate too small");

	PG_FREE_IF_COPY(arg, 0);
	PG_FREE_IF_COPY(name, 1);

	PG_RETURN_TEXT_P(res);
}

/* SQL function: decode(text, text) returns bytea */
PG_FUNCTION_INFO_V1(decode);

Datum
decode(PG_FUNCTION_ARGS)
{
	text	   *arg;
	text	   *name;
	uint		len,
				rlen,
				rlen0;
	pg_coding  *c,
				cbuf;
	text	   *res;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();

	name = PG_GETARG_TEXT_P(1);
	c = find_coding(&cbuf, name, 0);	/* will give error if fails */

	arg = PG_GETARG_TEXT_P(0);
	len = VARSIZE(arg) - VARHDRSZ;

	rlen0 = c->decode_len(len);

	res = (text *) palloc(rlen0 + VARHDRSZ);

	rlen = c->decode(VARDATA(arg), len, VARDATA(res));
	VARATT_SIZEP(res) = rlen + VARHDRSZ;

	if (rlen > rlen0)
		elog(FATAL, "pg_decode: overflow, decode estimate too small");

	PG_FREE_IF_COPY(arg, 0);
	PG_FREE_IF_COPY(name, 1);

	PG_RETURN_TEXT_P(res);
}

static pg_coding *
find_coding(pg_coding * dst, text *name, int silent)
{
	pg_coding  *p;
	char		buf[NAMEDATALEN];
	uint		len;

	len = VARSIZE(name) - VARHDRSZ;
	if (len >= NAMEDATALEN)
	{
		if (silent)
			return NULL;
		elog(ERROR, "Encoding type does not exist (name too long)");
	}

	memcpy(buf, VARDATA(name), len);
	buf[len] = 0;

	p = pg_find_coding(dst, buf);

	if (p == NULL && !silent)
		elog(ERROR, "Encoding type does not exist: '%s'", buf);
	return p;
}

static char *hextbl = "0123456789abcdef";

uint
hex_encode(uint8 *src, uint len, uint8 *dst)
{
	uint8	   *end = src + len;

	while (src < end)
	{
		*dst++ = hextbl[(*src >> 4) & 0xF];
		*dst++ = hextbl[*src & 0xF];
		src++;
	}
	return len * 2;
}

/* probably should use lookup table */
static uint8
get_hex(char c)
{
	uint8		res = 0;

	if (c >= '0' && c <= '9')
		res = c - '0';
	else if (c >= 'a' && c <= 'f')
		res = c - 'a' + 10;
	else if (c >= 'A' && c <= 'F')
		res = c - 'A' + 10;
	else
		elog(ERROR, "Bad hex code: '%c'", c);

	return res;
}

uint
hex_decode(uint8 *src, uint len, uint8 *dst)
{
	uint8	   *s,
			   *srcend,
				v1,
				v2,
			   *p = dst;

	srcend = src + len;
	s = src;
	p = dst;
	while (s < srcend)
	{
		if (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r')
		{
			s++;
			continue;
		}
		v1 = get_hex(*s++) << 4;
		if (s >= srcend)
			elog(ERROR, "hex_decode: invalid data");
		v2 = get_hex(*s++);
		*p++ = v1 | v2;
	}

	return p - dst;
}


static unsigned char _base64[] =
"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

uint
b64_encode(uint8 *src, uint len, uint8 *dst)
{
	uint8	   *s,
			   *p,
			   *end = src + len,
			   *lend = dst + 76;
	int			pos = 2;
	unsigned long buf = 0;

	s = src;
	p = dst;

	while (s < end)
	{
		buf |= *s << (pos << 3);
		pos--;
		s++;

		/* write it out */
		if (pos < 0)
		{
			*p++ = _base64[(buf >> 18) & 0x3f];
			*p++ = _base64[(buf >> 12) & 0x3f];
			*p++ = _base64[(buf >> 6) & 0x3f];
			*p++ = _base64[buf & 0x3f];

			pos = 2;
			buf = 0;
		}
		if (p >= lend)
		{
			*p++ = '\n';
			lend = p + 76;
		}
	}
	if (pos != 2)
	{
		*p++ = _base64[(buf >> 18) & 0x3f];
		*p++ = _base64[(buf >> 12) & 0x3f];
		*p++ = (pos == 0) ? _base64[(buf >> 6) & 0x3f] : '=';
		*p++ = '=';
	}

	return p - dst;
}

/* probably should use lookup table */
uint
b64_decode(uint8 *src, uint len, uint8 *dst)
{
	char	   *srcend = src + len,
			   *s = src;
	uint8	   *p = dst;
	char		c;
	uint		b = 0;
	unsigned long buf = 0;
	int			pos = 0,
				end = 0;

	while (s < srcend)
	{
		c = *s++;
		if (c >= 'A' && c <= 'Z')
			b = c - 'A';
		else if (c >= 'a' && c <= 'z')
			b = c - 'a' + 26;
		else if (c >= '0' && c <= '9')
			b = c - '0' + 52;
		else if (c == '+')
			b = 62;
		else if (c == '/')
			b = 63;
		else if (c == '=')
		{
			/* end sequence */
			if (!end)
			{
				if (pos == 2)
					end = 1;
				else if (pos == 3)
					end = 2;
				else
					elog(ERROR, "base64: unexpected '='");
			}
			b = 0;
		}
		else if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
			continue;
		else
			elog(ERROR, "base64: Invalid symbol");

		/* add it to buffer */
		buf = (buf << 6) + b;
		pos++;
		if (pos == 4)
		{
			*p++ = (buf >> 16) & 255;
			if (end == 0 || end > 1)
				*p++ = (buf >> 8) & 255;
			if (end == 0 || end > 2)
				*p++ = buf & 255;
			buf = 0;
			pos = 0;
		}
	}

	if (pos != 0)
		elog(ERROR, "base64: invalid end sequence");

	return p - dst;
}


uint
hex_enc_len(uint srclen)
{
	return srclen << 1;
}

uint
hex_dec_len(uint srclen)
{
	return srclen >> 1;
}

uint
b64_enc_len(uint srclen)
{
	return srclen + (srclen / 3) + (srclen / (76 / 2));
}

uint
b64_dec_len(uint srclen)
{
	return (srclen * 3) >> 2;
}

static pg_coding
			encoding_list[] = {
	{"hex", hex_enc_len, hex_dec_len, hex_encode, hex_decode},
	{"base64", b64_enc_len, b64_dec_len, b64_encode, b64_decode},
	{NULL, NULL, NULL, NULL, NULL}
};


static pg_coding *
pg_find_coding(pg_coding * res, char *name)
{
	pg_coding  *p;

	for (p = encoding_list; p->name; p++)
	{
		if (!strcasecmp(p->name, name))
			return p;
	}
	return NULL;
}

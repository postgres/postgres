/*
 * pgp-armor.c
 *		PGP ascii-armor.
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
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * contrib/pgcrypto/pgp-armor.c
 */

#include "postgres.h"

#include "pgp.h"
#include "px.h"

/*
 * BASE64 - duplicated :(
 */

static const unsigned char _base64[] =
"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int
pg_base64_encode(const uint8 *src, unsigned len, uint8 *dst)
{
	uint8	   *p,
			   *lend = dst + 76;
	const uint8 *s,
			   *end = src + len;
	int			pos = 2;
	unsigned long buf = 0;

	s = src;
	p = dst;

	while (s < end)
	{
		buf |= *s << (pos << 3);
		pos--;
		s++;

		/*
		 * write it out
		 */
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
static int
pg_base64_decode(const uint8 *src, unsigned len, uint8 *dst)
{
	const uint8 *srcend = src + len,
			   *s = src;
	uint8	   *p = dst;
	char		c;
	unsigned	b = 0;
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
			/*
			 * end sequence
			 */
			if (!end)
			{
				if (pos == 2)
					end = 1;
				else if (pos == 3)
					end = 2;
				else
					return PXE_PGP_CORRUPT_ARMOR;
			}
			b = 0;
		}
		else if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
			continue;
		else
			return PXE_PGP_CORRUPT_ARMOR;

		/*
		 * add it to buffer
		 */
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
		return PXE_PGP_CORRUPT_ARMOR;
	return p - dst;
}

static unsigned
pg_base64_enc_len(unsigned srclen)
{
	/*
	 * 3 bytes will be converted to 4, linefeed after 76 chars
	 */
	return (srclen + 2) / 3 * 4 + srclen / (76 * 3 / 4);
}

static unsigned
pg_base64_dec_len(unsigned srclen)
{
	return (srclen * 3) >> 2;
}

/*
 * PGP armor
 */

static const char *const armor_header = "-----BEGIN PGP MESSAGE-----\n";
static const char *const armor_footer = "\n-----END PGP MESSAGE-----\n";

/* CRC24 implementation from rfc2440 */
#define CRC24_INIT 0x00b704ceL
#define CRC24_POLY 0x01864cfbL
static long
crc24(const uint8 *data, unsigned len)
{
	unsigned	crc = CRC24_INIT;
	int			i;

	while (len--)
	{
		crc ^= (*data++) << 16;
		for (i = 0; i < 8; i++)
		{
			crc <<= 1;
			if (crc & 0x1000000)
				crc ^= CRC24_POLY;
		}
	}
	return crc & 0xffffffL;
}

void
pgp_armor_encode(const uint8 *src, unsigned len, StringInfo dst,
				 int num_headers, char **keys, char **values)
{
	int			n;
	int			res;
	unsigned	b64len;
	unsigned	crc = crc24(src, len);

	appendStringInfoString(dst, armor_header);

	for (n = 0; n < num_headers; n++)
		appendStringInfo(dst, "%s: %s\n", keys[n], values[n]);
	appendStringInfoChar(dst, '\n');

	/* make sure we have enough room to pg_base64_encode() */
	b64len = pg_base64_enc_len(len);
	enlargeStringInfo(dst, (int) b64len);

	res = pg_base64_encode(src, len, (uint8 *) dst->data + dst->len);
	if (res > b64len)
		elog(FATAL, "overflow - encode estimate too small");
	dst->len += res;

	if (*(dst->data + dst->len - 1) != '\n')
		appendStringInfoChar(dst, '\n');

	appendStringInfoChar(dst, '=');
	appendStringInfoChar(dst, _base64[(crc >> 18) & 0x3f]);
	appendStringInfoChar(dst, _base64[(crc >> 12) & 0x3f]);
	appendStringInfoChar(dst, _base64[(crc >> 6) & 0x3f]);
	appendStringInfoChar(dst, _base64[crc & 0x3f]);

	appendStringInfoString(dst, armor_footer);
}

static const uint8 *
find_str(const uint8 *data, const uint8 *data_end, const char *str, int strlen)
{
	const uint8 *p = data;

	if (!strlen)
		return NULL;
	if (data_end - data < strlen)
		return NULL;
	while (p < data_end)
	{
		p = memchr(p, str[0], data_end - p);
		if (p == NULL)
			return NULL;
		if (p + strlen > data_end)
			return NULL;
		if (memcmp(p, str, strlen) == 0)
			return p;
		p++;
	}
	return NULL;
}

static int
find_header(const uint8 *data, const uint8 *datend,
			const uint8 **start_p, int is_end)
{
	const uint8 *p = data;
	static const char *start_sep = "-----BEGIN";
	static const char *end_sep = "-----END";
	const char *sep = is_end ? end_sep : start_sep;

	/* find header line */
	while (1)
	{
		p = find_str(p, datend, sep, strlen(sep));
		if (p == NULL)
			return PXE_PGP_CORRUPT_ARMOR;
		/* it must start at beginning of line */
		if (p == data || *(p - 1) == '\n')
			break;
		p += strlen(sep);
	}
	*start_p = p;
	p += strlen(sep);

	/* check if header text ok */
	for (; p < datend && *p != '-'; p++)
	{
		/* various junk can be there, but definitely not line-feed	*/
		if (*p >= ' ')
			continue;
		return PXE_PGP_CORRUPT_ARMOR;
	}
	if (datend - p < 5 || memcmp(p, sep, 5) != 0)
		return PXE_PGP_CORRUPT_ARMOR;
	p += 5;

	/* check if at end of line */
	if (p < datend)
	{
		if (*p != '\n' && *p != '\r')
			return PXE_PGP_CORRUPT_ARMOR;
		if (*p == '\r')
			p++;
		if (p < datend && *p == '\n')
			p++;
	}
	return p - *start_p;
}

int
pgp_armor_decode(const uint8 *src, int len, StringInfo dst)
{
	const uint8 *p = src;
	const uint8 *data_end = src + len;
	long		crc;
	const uint8 *base64_start,
			   *armor_end;
	const uint8 *base64_end = NULL;
	uint8		buf[4];
	int			hlen;
	int			blen;
	int			res = PXE_PGP_CORRUPT_ARMOR;

	/* armor start */
	hlen = find_header(src, data_end, &p, 0);
	if (hlen <= 0)
		goto out;
	p += hlen;

	/* armor end */
	hlen = find_header(p, data_end, &armor_end, 1);
	if (hlen <= 0)
		goto out;

	/* skip comments - find empty line */
	while (p < armor_end && *p != '\n' && *p != '\r')
	{
		p = memchr(p, '\n', armor_end - p);
		if (!p)
			goto out;

		/* step to start of next line */
		p++;
	}
	base64_start = p;

	/* find crc pos */
	for (p = armor_end; p >= base64_start; p--)
		if (*p == '=')
		{
			base64_end = p - 1;
			break;
		}
	if (base64_end == NULL)
		goto out;

	/* decode crc */
	if (pg_base64_decode(p + 1, 4, buf) != 3)
		goto out;
	crc = (((long) buf[0]) << 16) + (((long) buf[1]) << 8) + (long) buf[2];

	/* decode data */
	blen = (int) pg_base64_dec_len(len);
	enlargeStringInfo(dst, blen);
	res = pg_base64_decode(base64_start, base64_end - base64_start, (uint8 *) dst->data);
	if (res > blen)
		elog(FATAL, "overflow - decode estimate too small");
	if (res >= 0)
	{
		if (crc24((uint8 *) dst->data, res) == crc)
			dst->len += res;
		else
			res = PXE_PGP_CORRUPT_ARMOR;
	}
out:
	return res;
}

/*
 * Extracts all armor headers from an ASCII-armored input.
 *
 * Returns 0 on success, or PXE_* error code on error. On success, the
 * number of headers and their keys and values are returned in *nheaders,
 * *nkeys and *nvalues.
 */
int
pgp_extract_armor_headers(const uint8 *src, unsigned len,
						  int *nheaders, char ***keys, char ***values)
{
	const uint8 *data_end = src + len;
	const uint8 *p;
	const uint8 *base64_start;
	const uint8 *armor_start;
	const uint8 *armor_end;
	Size		armor_len;
	char	   *line;
	char	   *nextline;
	char	   *eol,
			   *colon;
	int			hlen;
	char	   *buf;
	int			hdrlines;
	int			n;

	/* armor start */
	hlen = find_header(src, data_end, &armor_start, 0);
	if (hlen <= 0)
		return PXE_PGP_CORRUPT_ARMOR;
	armor_start += hlen;

	/* armor end */
	hlen = find_header(armor_start, data_end, &armor_end, 1);
	if (hlen <= 0)
		return PXE_PGP_CORRUPT_ARMOR;

	/* Count the number of armor header lines. */
	hdrlines = 0;
	p = armor_start;
	while (p < armor_end && *p != '\n' && *p != '\r')
	{
		p = memchr(p, '\n', armor_end - p);
		if (!p)
			return PXE_PGP_CORRUPT_ARMOR;

		/* step to start of next line */
		p++;
		hdrlines++;
	}
	base64_start = p;

	/*
	 * Make a modifiable copy of the part of the input that contains the
	 * headers. The returned key/value pointers will point inside the buffer.
	 */
	armor_len = base64_start - armor_start;
	buf = palloc(armor_len + 1);
	memcpy(buf, armor_start, armor_len);
	buf[armor_len] = '\0';

	/* Allocate return arrays */
	*keys = (char **) palloc(hdrlines * sizeof(char *));
	*values = (char **) palloc(hdrlines * sizeof(char *));

	/*
	 * Split the header lines at newlines and ": " separators, and collect
	 * pointers to the keys and values in the return arrays.
	 */
	n = 0;
	line = buf;
	for (;;)
	{
		/* find end of line */
		eol = strchr(line, '\n');
		if (!eol)
			break;
		nextline = eol + 1;
		/* if the line ends in CR + LF, strip the CR */
		if (eol > line && *(eol - 1) == '\r')
			eol--;
		*eol = '\0';

		/* find colon+space separating the key and value */
		colon = strstr(line, ": ");
		if (!colon)
			return PXE_PGP_CORRUPT_ARMOR;
		*colon = '\0';

		/* shouldn't happen, we counted the number of lines beforehand */
		if (n >= hdrlines)
			elog(ERROR, "unexpected number of armor header lines");

		(*keys)[n] = line;
		(*values)[n] = colon + 2;
		n++;

		/* step to start of next line */
		line = nextline;
	}

	if (n != hdrlines)
		elog(ERROR, "unexpected number of armor header lines");

	*nheaders = n;
	return 0;
}

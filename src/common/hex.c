/*-------------------------------------------------------------------------
 *
 * hex.c
 *	  Encoding and decoding routines for hex.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/common/hex.c
 *
 *-------------------------------------------------------------------------
 */


#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/hex.h"
#ifdef FRONTEND
#include "common/logging.h"
#endif
#include "mb/pg_wchar.h"


static const int8 hexlookup[128] = {
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1, -1, -1, -1, -1, -1,
	-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

static const char hextbl[] = "0123456789abcdef";

static inline char
get_hex(const char *cp)
{
	unsigned char c = (unsigned char) *cp;
	int			res = -1;

	if (c < 127)
		res = hexlookup[c];

	if (res < 0)
	{
#ifdef FRONTEND
		pg_log_fatal("invalid hexadecimal digit");
		exit(EXIT_FAILURE);
#else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid hexadecimal digit: \"%.*s\"",
						pg_mblen(cp), cp)));
#endif
	}

	return (char) res;
}

/*
 * pg_hex_encode
 *
 * Encode into hex the given string.  Returns the length of the encoded
 * string.
 */
uint64
pg_hex_encode(const char *src, size_t srclen, char *dst, size_t dstlen)
{
	const char *end = src + srclen;
	char	   *p;

	p = dst;

	while (src < end)
	{
		/*
		 * Leave if there is an overflow in the area allocated for the encoded
		 * string.
		 */
		if ((p - dst + 2) > dstlen)
		{
#ifdef FRONTEND
			pg_log_fatal("overflow of destination buffer in hex encoding");
			exit(EXIT_FAILURE);
#else
			elog(ERROR, "overflow of destination buffer in hex encoding");
#endif
		}

		*p++ = hextbl[(*src >> 4) & 0xF];
		*p++ = hextbl[*src & 0xF];
		src++;
	}

	Assert((p - dst) <= dstlen);
	return p - dst;
}

/*
 * pg_hex_decode
 *
 * Decode the given hex string.  Returns the length of the decoded string.
 */
uint64
pg_hex_decode(const char *src, size_t srclen, char *dst, size_t dstlen)
{
	const char *s,
			   *srcend;
	char		v1,
				v2,
			   *p;

	srcend = src + srclen;
	s = src;
	p = dst;
	while (s < srcend)
	{
		if (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r')
		{
			s++;
			continue;
		}
		v1 = get_hex(s) << 4;
		s++;

		if (s >= srcend)
		{
#ifdef FRONTEND
			pg_log_fatal("invalid hexadecimal data: odd number of digits");
			exit(EXIT_FAILURE);
#else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid hexadecimal data: odd number of digits")));
#endif
		}

		v2 = get_hex(s);
		s++;

		/* overflow check */
		if ((p - dst + 1) > dstlen)
		{
#ifdef FRONTEND
			pg_log_fatal("overflow of destination buffer in hex decoding");
			exit(EXIT_FAILURE);
#else
			elog(ERROR, "overflow of destination buffer in hex decoding");
#endif
		}

		*p++ = v1 | v2;
	}

	Assert((p - dst) <= dstlen);
	return p - dst;
}

/*
 * pg_hex_enc_len
 *
 * Returns to caller the length of the string if it were encoded with
 * hex based on the length provided by caller.  This is useful to estimate
 * how large a buffer allocation needs to be done before doing the actual
 * encoding.
 */
uint64
pg_hex_enc_len(size_t srclen)
{
	return (uint64) srclen << 1;
}

/*
 * pg_hex_dec_len
 *
 * Returns to caller the length of the string if it were to be decoded
 * with hex, based on the length given by caller.  This is useful to
 * estimate how large a buffer allocation needs to be done before doing
 * the actual decoding.
 */
uint64
pg_hex_dec_len(size_t srclen)
{
	return (uint64) srclen >> 1;
}

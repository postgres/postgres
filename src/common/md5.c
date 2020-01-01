/*
 *	md5.c
 *
 *	Implements	the  MD5 Message-Digest Algorithm as specified in
 *	RFC  1321.  This  implementation  is a simple one, in that it
 *	needs  every  input  byte  to  be  buffered  before doing any
 *	calculations.  I  do  not  expect  this  file  to be used for
 *	general  purpose  MD5'ing  of large amounts of data, only for
 *	generating hashed passwords from limited input.
 *
 *	Sverre H. Huseby <sverrehu@online.no>
 *
 *	Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 *	Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/common/md5.c
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/md5.h"


/*
 *	PRIVATE FUNCTIONS
 */


/*
 *	The returned array is allocated using malloc.  the caller should free it
 *	when it is no longer needed.
 */
static uint8 *
createPaddedCopyWithLength(const uint8 *b, uint32 *l)
{
	uint8	   *ret;
	uint32		q;
	uint32		len,
				newLen448;
	uint32		len_high,
				len_low;		/* 64-bit value split into 32-bit sections */

	len = ((b == NULL) ? 0 : *l);
	newLen448 = len + 64 - (len % 64) - 8;
	if (newLen448 <= len)
		newLen448 += 64;

	*l = newLen448 + 8;
	if ((ret = (uint8 *) malloc(sizeof(uint8) * *l)) == NULL)
		return NULL;

	if (b != NULL)
		memcpy(ret, b, sizeof(uint8) * len);

	/* pad */
	ret[len] = 0x80;
	for (q = len + 1; q < newLen448; q++)
		ret[q] = 0x00;

	/* append length as a 64 bit bitcount */
	len_low = len;
	/* split into two 32-bit values */
	/* we only look at the bottom 32-bits */
	len_high = len >> 29;
	len_low <<= 3;
	q = newLen448;
	ret[q++] = (len_low & 0xff);
	len_low >>= 8;
	ret[q++] = (len_low & 0xff);
	len_low >>= 8;
	ret[q++] = (len_low & 0xff);
	len_low >>= 8;
	ret[q++] = (len_low & 0xff);
	ret[q++] = (len_high & 0xff);
	len_high >>= 8;
	ret[q++] = (len_high & 0xff);
	len_high >>= 8;
	ret[q++] = (len_high & 0xff);
	len_high >>= 8;
	ret[q] = (len_high & 0xff);

	return ret;
}

#define F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | ~(z)))
#define ROT_LEFT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void
doTheRounds(uint32 X[16], uint32 state[4])
{
	uint32		a,
				b,
				c,
				d;

	a = state[0];
	b = state[1];
	c = state[2];
	d = state[3];

	/* round 1 */
	a = b + ROT_LEFT((a + F(b, c, d) + X[0] + 0xd76aa478), 7);	/* 1 */
	d = a + ROT_LEFT((d + F(a, b, c) + X[1] + 0xe8c7b756), 12); /* 2 */
	c = d + ROT_LEFT((c + F(d, a, b) + X[2] + 0x242070db), 17); /* 3 */
	b = c + ROT_LEFT((b + F(c, d, a) + X[3] + 0xc1bdceee), 22); /* 4 */
	a = b + ROT_LEFT((a + F(b, c, d) + X[4] + 0xf57c0faf), 7);	/* 5 */
	d = a + ROT_LEFT((d + F(a, b, c) + X[5] + 0x4787c62a), 12); /* 6 */
	c = d + ROT_LEFT((c + F(d, a, b) + X[6] + 0xa8304613), 17); /* 7 */
	b = c + ROT_LEFT((b + F(c, d, a) + X[7] + 0xfd469501), 22); /* 8 */
	a = b + ROT_LEFT((a + F(b, c, d) + X[8] + 0x698098d8), 7);	/* 9 */
	d = a + ROT_LEFT((d + F(a, b, c) + X[9] + 0x8b44f7af), 12); /* 10 */
	c = d + ROT_LEFT((c + F(d, a, b) + X[10] + 0xffff5bb1), 17);	/* 11 */
	b = c + ROT_LEFT((b + F(c, d, a) + X[11] + 0x895cd7be), 22);	/* 12 */
	a = b + ROT_LEFT((a + F(b, c, d) + X[12] + 0x6b901122), 7); /* 13 */
	d = a + ROT_LEFT((d + F(a, b, c) + X[13] + 0xfd987193), 12);	/* 14 */
	c = d + ROT_LEFT((c + F(d, a, b) + X[14] + 0xa679438e), 17);	/* 15 */
	b = c + ROT_LEFT((b + F(c, d, a) + X[15] + 0x49b40821), 22);	/* 16 */

	/* round 2 */
	a = b + ROT_LEFT((a + G(b, c, d) + X[1] + 0xf61e2562), 5);	/* 17 */
	d = a + ROT_LEFT((d + G(a, b, c) + X[6] + 0xc040b340), 9);	/* 18 */
	c = d + ROT_LEFT((c + G(d, a, b) + X[11] + 0x265e5a51), 14);	/* 19 */
	b = c + ROT_LEFT((b + G(c, d, a) + X[0] + 0xe9b6c7aa), 20); /* 20 */
	a = b + ROT_LEFT((a + G(b, c, d) + X[5] + 0xd62f105d), 5);	/* 21 */
	d = a + ROT_LEFT((d + G(a, b, c) + X[10] + 0x02441453), 9); /* 22 */
	c = d + ROT_LEFT((c + G(d, a, b) + X[15] + 0xd8a1e681), 14);	/* 23 */
	b = c + ROT_LEFT((b + G(c, d, a) + X[4] + 0xe7d3fbc8), 20); /* 24 */
	a = b + ROT_LEFT((a + G(b, c, d) + X[9] + 0x21e1cde6), 5);	/* 25 */
	d = a + ROT_LEFT((d + G(a, b, c) + X[14] + 0xc33707d6), 9); /* 26 */
	c = d + ROT_LEFT((c + G(d, a, b) + X[3] + 0xf4d50d87), 14); /* 27 */
	b = c + ROT_LEFT((b + G(c, d, a) + X[8] + 0x455a14ed), 20); /* 28 */
	a = b + ROT_LEFT((a + G(b, c, d) + X[13] + 0xa9e3e905), 5); /* 29 */
	d = a + ROT_LEFT((d + G(a, b, c) + X[2] + 0xfcefa3f8), 9);	/* 30 */
	c = d + ROT_LEFT((c + G(d, a, b) + X[7] + 0x676f02d9), 14); /* 31 */
	b = c + ROT_LEFT((b + G(c, d, a) + X[12] + 0x8d2a4c8a), 20);	/* 32 */

	/* round 3 */
	a = b + ROT_LEFT((a + H(b, c, d) + X[5] + 0xfffa3942), 4);	/* 33 */
	d = a + ROT_LEFT((d + H(a, b, c) + X[8] + 0x8771f681), 11); /* 34 */
	c = d + ROT_LEFT((c + H(d, a, b) + X[11] + 0x6d9d6122), 16);	/* 35 */
	b = c + ROT_LEFT((b + H(c, d, a) + X[14] + 0xfde5380c), 23);	/* 36 */
	a = b + ROT_LEFT((a + H(b, c, d) + X[1] + 0xa4beea44), 4);	/* 37 */
	d = a + ROT_LEFT((d + H(a, b, c) + X[4] + 0x4bdecfa9), 11); /* 38 */
	c = d + ROT_LEFT((c + H(d, a, b) + X[7] + 0xf6bb4b60), 16); /* 39 */
	b = c + ROT_LEFT((b + H(c, d, a) + X[10] + 0xbebfbc70), 23);	/* 40 */
	a = b + ROT_LEFT((a + H(b, c, d) + X[13] + 0x289b7ec6), 4); /* 41 */
	d = a + ROT_LEFT((d + H(a, b, c) + X[0] + 0xeaa127fa), 11); /* 42 */
	c = d + ROT_LEFT((c + H(d, a, b) + X[3] + 0xd4ef3085), 16); /* 43 */
	b = c + ROT_LEFT((b + H(c, d, a) + X[6] + 0x04881d05), 23); /* 44 */
	a = b + ROT_LEFT((a + H(b, c, d) + X[9] + 0xd9d4d039), 4);	/* 45 */
	d = a + ROT_LEFT((d + H(a, b, c) + X[12] + 0xe6db99e5), 11);	/* 46 */
	c = d + ROT_LEFT((c + H(d, a, b) + X[15] + 0x1fa27cf8), 16);	/* 47 */
	b = c + ROT_LEFT((b + H(c, d, a) + X[2] + 0xc4ac5665), 23); /* 48 */

	/* round 4 */
	a = b + ROT_LEFT((a + I(b, c, d) + X[0] + 0xf4292244), 6);	/* 49 */
	d = a + ROT_LEFT((d + I(a, b, c) + X[7] + 0x432aff97), 10); /* 50 */
	c = d + ROT_LEFT((c + I(d, a, b) + X[14] + 0xab9423a7), 15);	/* 51 */
	b = c + ROT_LEFT((b + I(c, d, a) + X[5] + 0xfc93a039), 21); /* 52 */
	a = b + ROT_LEFT((a + I(b, c, d) + X[12] + 0x655b59c3), 6); /* 53 */
	d = a + ROT_LEFT((d + I(a, b, c) + X[3] + 0x8f0ccc92), 10); /* 54 */
	c = d + ROT_LEFT((c + I(d, a, b) + X[10] + 0xffeff47d), 15);	/* 55 */
	b = c + ROT_LEFT((b + I(c, d, a) + X[1] + 0x85845dd1), 21); /* 56 */
	a = b + ROT_LEFT((a + I(b, c, d) + X[8] + 0x6fa87e4f), 6);	/* 57 */
	d = a + ROT_LEFT((d + I(a, b, c) + X[15] + 0xfe2ce6e0), 10);	/* 58 */
	c = d + ROT_LEFT((c + I(d, a, b) + X[6] + 0xa3014314), 15); /* 59 */
	b = c + ROT_LEFT((b + I(c, d, a) + X[13] + 0x4e0811a1), 21);	/* 60 */
	a = b + ROT_LEFT((a + I(b, c, d) + X[4] + 0xf7537e82), 6);	/* 61 */
	d = a + ROT_LEFT((d + I(a, b, c) + X[11] + 0xbd3af235), 10);	/* 62 */
	c = d + ROT_LEFT((c + I(d, a, b) + X[2] + 0x2ad7d2bb), 15); /* 63 */
	b = c + ROT_LEFT((b + I(c, d, a) + X[9] + 0xeb86d391), 21); /* 64 */

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
}

static int
calculateDigestFromBuffer(const uint8 *b, uint32 len, uint8 sum[16])
{
	register uint32 i,
				j,
				k,
				newI;
	uint32		l;
	uint8	   *input;
	register uint32 *wbp;
	uint32		workBuff[16],
				state[4];

	l = len;

	state[0] = 0x67452301;
	state[1] = 0xEFCDAB89;
	state[2] = 0x98BADCFE;
	state[3] = 0x10325476;

	if ((input = createPaddedCopyWithLength(b, &l)) == NULL)
		return 0;

	for (i = 0;;)
	{
		if ((newI = i + 16 * 4) > l)
			break;
		k = i + 3;
		for (j = 0; j < 16; j++)
		{
			wbp = (workBuff + j);
			*wbp = input[k--];
			*wbp <<= 8;
			*wbp |= input[k--];
			*wbp <<= 8;
			*wbp |= input[k--];
			*wbp <<= 8;
			*wbp |= input[k];
			k += 7;
		}
		doTheRounds(workBuff, state);
		i = newI;
	}
	free(input);

	j = 0;
	for (i = 0; i < 4; i++)
	{
		k = state[i];
		sum[j++] = (k & 0xff);
		k >>= 8;
		sum[j++] = (k & 0xff);
		k >>= 8;
		sum[j++] = (k & 0xff);
		k >>= 8;
		sum[j++] = (k & 0xff);
	}
	return 1;
}

static void
bytesToHex(uint8 b[16], char *s)
{
	static const char *hex = "0123456789abcdef";
	int			q,
				w;

	for (q = 0, w = 0; q < 16; q++)
	{
		s[w++] = hex[(b[q] >> 4) & 0x0F];
		s[w++] = hex[b[q] & 0x0F];
	}
	s[w] = '\0';
}

/*
 *	PUBLIC FUNCTIONS
 */

/*
 *	pg_md5_hash
 *
 *	Calculates the MD5 sum of the bytes in a buffer.
 *
 *	SYNOPSIS	  #include "md5.h"
 *				  int pg_md5_hash(const void *buff, size_t len, char *hexsum)
 *
 *	INPUT		  buff	  the buffer containing the bytes that you want
 *						  the MD5 sum of.
 *				  len	  number of bytes in the buffer.
 *
 *	OUTPUT		  hexsum  the MD5 sum as a '\0'-terminated string of
 *						  hexadecimal digits.  an MD5 sum is 16 bytes long.
 *						  each byte is represented by two hexadecimal
 *						  characters.  you thus need to provide an array
 *						  of 33 characters, including the trailing '\0'.
 *
 *	RETURNS		  false on failure (out of memory for internal buffers) or
 *				  true on success.
 *
 *	STANDARDS	  MD5 is described in RFC 1321.
 *
 *	AUTHOR		  Sverre H. Huseby <sverrehu@online.no>
 *
 */
bool
pg_md5_hash(const void *buff, size_t len, char *hexsum)
{
	uint8		sum[16];

	if (!calculateDigestFromBuffer(buff, len, sum))
		return false;

	bytesToHex(sum, hexsum);
	return true;
}

bool
pg_md5_binary(const void *buff, size_t len, void *outbuf)
{
	if (!calculateDigestFromBuffer(buff, len, outbuf))
		return false;
	return true;
}


/*
 * Computes MD5 checksum of "passwd" (a null-terminated string) followed
 * by "salt" (which need not be null-terminated).
 *
 * Output format is "md5" followed by a 32-hex-digit MD5 checksum.
 * Hence, the output buffer "buf" must be at least 36 bytes long.
 *
 * Returns true if okay, false on error (out of memory).
 */
bool
pg_md5_encrypt(const char *passwd, const char *salt, size_t salt_len,
			   char *buf)
{
	size_t		passwd_len = strlen(passwd);

	/* +1 here is just to avoid risk of unportable malloc(0) */
	char	   *crypt_buf = malloc(passwd_len + salt_len + 1);
	bool		ret;

	if (!crypt_buf)
		return false;

	/*
	 * Place salt at the end because it may be known by users trying to crack
	 * the MD5 output.
	 */
	memcpy(crypt_buf, passwd, passwd_len);
	memcpy(crypt_buf + passwd_len, salt, salt_len);

	strcpy(buf, "md5");
	ret = pg_md5_hash(crypt_buf, passwd_len + salt_len, buf + 3);

	free(crypt_buf);

	return ret;
}

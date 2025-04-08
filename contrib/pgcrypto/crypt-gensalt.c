/*
 * Written by Solar Designer and placed in the public domain.
 * See crypt_blowfish.c for more information.
 *
 * contrib/pgcrypto/crypt-gensalt.c
 *
 * This file contains salt generation functions for the traditional and
 * other common crypt(3) algorithms, except for bcrypt which is defined
 * entirely in crypt_blowfish.c.
 *
 * Put bcrypt generator also here as crypt-blowfish.c
 * may not be compiled always.        -- marko
 */

#include "postgres.h"

#include "px-crypt.h"

typedef unsigned int BF_word;

static unsigned char _crypt_itoa64[64 + 1] =
"./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

char *
_crypt_gensalt_traditional_rn(unsigned long count,
							  const char *input, int size, char *output, int output_size)
{
	if (size < 2 || output_size < 2 + 1 || (count && count != 25))
	{
		if (output_size > 0)
			output[0] = '\0';
		return NULL;
	}

	output[0] = _crypt_itoa64[(unsigned int) input[0] & 0x3f];
	output[1] = _crypt_itoa64[(unsigned int) input[1] & 0x3f];
	output[2] = '\0';

	return output;
}

char *
_crypt_gensalt_extended_rn(unsigned long count,
						   const char *input, int size, char *output, int output_size)
{
	unsigned long value;

/* Even iteration counts make it easier to detect weak DES keys from a look
 * at the hash, so they should be avoided */
	if (size < 3 || output_size < 1 + 4 + 4 + 1 ||
		(count && (count > 0xffffff || !(count & 1))))
	{
		if (output_size > 0)
			output[0] = '\0';
		return NULL;
	}

	if (!count)
		count = 725;

	output[0] = '_';
	output[1] = _crypt_itoa64[count & 0x3f];
	output[2] = _crypt_itoa64[(count >> 6) & 0x3f];
	output[3] = _crypt_itoa64[(count >> 12) & 0x3f];
	output[4] = _crypt_itoa64[(count >> 18) & 0x3f];
	value = (unsigned long) (unsigned char) input[0] |
		((unsigned long) (unsigned char) input[1] << 8) |
		((unsigned long) (unsigned char) input[2] << 16);
	output[5] = _crypt_itoa64[value & 0x3f];
	output[6] = _crypt_itoa64[(value >> 6) & 0x3f];
	output[7] = _crypt_itoa64[(value >> 12) & 0x3f];
	output[8] = _crypt_itoa64[(value >> 18) & 0x3f];
	output[9] = '\0';

	return output;
}

char *
_crypt_gensalt_md5_rn(unsigned long count,
					  const char *input, int size, char *output, int output_size)
{
	unsigned long value;

	if (size < 3 || output_size < 3 + 4 + 1 || (count && count != 1000))
	{
		if (output_size > 0)
			output[0] = '\0';
		return NULL;
	}

	output[0] = '$';
	output[1] = '1';
	output[2] = '$';
	value = (unsigned long) (unsigned char) input[0] |
		((unsigned long) (unsigned char) input[1] << 8) |
		((unsigned long) (unsigned char) input[2] << 16);
	output[3] = _crypt_itoa64[value & 0x3f];
	output[4] = _crypt_itoa64[(value >> 6) & 0x3f];
	output[5] = _crypt_itoa64[(value >> 12) & 0x3f];
	output[6] = _crypt_itoa64[(value >> 18) & 0x3f];
	output[7] = '\0';

	if (size >= 6 && output_size >= 3 + 4 + 4 + 1)
	{
		value = (unsigned long) (unsigned char) input[3] |
			((unsigned long) (unsigned char) input[4] << 8) |
			((unsigned long) (unsigned char) input[5] << 16);
		output[7] = _crypt_itoa64[value & 0x3f];
		output[8] = _crypt_itoa64[(value >> 6) & 0x3f];
		output[9] = _crypt_itoa64[(value >> 12) & 0x3f];
		output[10] = _crypt_itoa64[(value >> 18) & 0x3f];
		output[11] = '\0';
	}

	return output;
}



static unsigned char BF_itoa64[64 + 1] =
"./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

static void
BF_encode(char *dst, const BF_word *src, int size)
{
	const unsigned char *sptr = (const unsigned char *) src;
	const unsigned char *end = sptr + size;
	unsigned char *dptr = (unsigned char *) dst;
	unsigned int c1,
				c2;

	do
	{
		c1 = *sptr++;
		*dptr++ = BF_itoa64[c1 >> 2];
		c1 = (c1 & 0x03) << 4;
		if (sptr >= end)
		{
			*dptr++ = BF_itoa64[c1];
			break;
		}

		c2 = *sptr++;
		c1 |= c2 >> 4;
		*dptr++ = BF_itoa64[c1];
		c1 = (c2 & 0x0f) << 2;
		if (sptr >= end)
		{
			*dptr++ = BF_itoa64[c1];
			break;
		}

		c2 = *sptr++;
		c1 |= c2 >> 6;
		*dptr++ = BF_itoa64[c1];
		*dptr++ = BF_itoa64[c2 & 0x3f];
	} while (sptr < end);
}

char *
_crypt_gensalt_blowfish_rn(unsigned long count,
						   const char *input, int size, char *output, int output_size)
{
	if (size < 16 || output_size < 7 + 22 + 1 ||
		(count && (count < 4 || count > 31)))
	{
		if (output_size > 0)
			output[0] = '\0';
		return NULL;
	}

	if (!count)
		count = 5;

	output[0] = '$';
	output[1] = '2';
	output[2] = 'a';
	output[3] = '$';
	output[4] = '0' + count / 10;
	output[5] = '0' + count % 10;
	output[6] = '$';

	BF_encode(&output[7], (const BF_word *) input, 16);
	output[7 + 22] = '\0';

	return output;
}

/*
 * Helper for _crypt_gensalt_sha256_rn and _crypt_gensalt_sha512_rn
 */
static char *
_crypt_gensalt_sha(unsigned long count,
				   const char *input, int size, char *output, int output_size)
{
	char	   *s_ptr = output;
	unsigned int result_bufsize = PX_SHACRYPT_SALT_BUF_LEN;
	int			rc;

	/* output buffer must be allocated with PX_MAX_SALT_LEN bytes */
	if (PX_MAX_SALT_LEN < result_bufsize)
		ereport(ERROR,
				errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("invalid size of salt"));

	/*
	 * Care must be taken to not exceed the buffer size allocated for the
	 * input character buffer.
	 */
	if ((PX_SHACRYPT_SALT_MAX_LEN != size) || (output_size < size))
		ereport(ERROR,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("invalid length of salt buffer"));

	/* Skip magic bytes, set by callers */
	s_ptr += 3;
	if ((rc = pg_snprintf(s_ptr, 18, "rounds=%lu$", count)) <= 0)
		ereport(ERROR,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("cannot format salt string"));

	/* s_ptr should now be positioned at the start of the salt string */
	s_ptr += rc;

	/*
	 * Normalize salt string
	 *
	 * size of input buffer was checked above to not exceed
	 * PX_SHACRYPT_SALT_LEN_MAX.
	 */
	for (int i = 0; i < size; i++)
	{
		*s_ptr = _crypt_itoa64[input[i] & 0x3f];
		s_ptr++;
	}

	/* We're done */
	return output;
}

/* gen_list->gen function for sha512 */
char *
_crypt_gensalt_sha512_rn(unsigned long count,
						 char const *input, int size,
						 char *output, int output_size)
{
	memset(output, 0, output_size);
	/* set magic byte for sha512crypt */
	output[0] = '$';
	output[1] = '6';
	output[2] = '$';

	return _crypt_gensalt_sha(count, input, size, output, output_size);
}

/* gen_list->gen function for sha256 */
char *
_crypt_gensalt_sha256_rn(unsigned long count,
						 const char *input, int size,
						 char *output, int output_size)
{
	memset(output, 0, output_size);
	/* set magic byte for sha256crypt */
	output[0] = '$';
	output[1] = '5';
	output[2] = '$';

	return _crypt_gensalt_sha(count, input, size, output, output_size);
}

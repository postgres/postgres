/*-------------------------------------------------------------------------
 *
 * md5_common.c
 *	  Routines shared between all MD5 implementations used for encrypted
 *	  passwords.
 *
 * Sverre H. Huseby <sverrehu@online.no>
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/common/md5_common.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/cryptohash.h"
#include "common/md5.h"

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
 *	pg_md5_hash
 *
 *	Calculates the MD5 sum of the bytes in a buffer.
 *
 *	SYNOPSIS	  #include "md5.h"
 *				  bool pg_md5_hash(const void *buff, size_t len, char *hexsum,
 *				                   const char **errstr)
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
 *				  errstr  filled with a constant-string error message
 *						  on failure return; NULL on success.
 *
 *	RETURNS		  false on failure (out of memory for internal buffers
 *				  or MD5 computation failure) or true on success.
 *
 *	STANDARDS	  MD5 is described in RFC 1321.
 *
 *	AUTHOR		  Sverre H. Huseby <sverrehu@online.no>
 *
 */

bool
pg_md5_hash(const void *buff, size_t len, char *hexsum, const char **errstr)
{
	uint8		sum[MD5_DIGEST_LENGTH];
	pg_cryptohash_ctx *ctx;

	*errstr = NULL;
	ctx = pg_cryptohash_create(PG_MD5);
	if (ctx == NULL)
	{
		*errstr = pg_cryptohash_error(NULL);	/* returns OOM */
		return false;
	}

	if (pg_cryptohash_init(ctx) < 0 ||
		pg_cryptohash_update(ctx, buff, len) < 0 ||
		pg_cryptohash_final(ctx, sum, sizeof(sum)) < 0)
	{
		*errstr = pg_cryptohash_error(ctx);
		pg_cryptohash_free(ctx);
		return false;
	}

	bytesToHex(sum, hexsum);
	pg_cryptohash_free(ctx);
	return true;
}

/*
 * pg_md5_binary
 *
 * As above, except that the MD5 digest is returned as a binary string
 * (of size MD5_DIGEST_LENGTH) rather than being converted to ASCII hex.
 */
bool
pg_md5_binary(const void *buff, size_t len, void *outbuf, const char **errstr)
{
	pg_cryptohash_ctx *ctx;

	*errstr = NULL;
	ctx = pg_cryptohash_create(PG_MD5);
	if (ctx == NULL)
	{
		*errstr = pg_cryptohash_error(NULL);	/* returns OOM */
		return false;
	}

	if (pg_cryptohash_init(ctx) < 0 ||
		pg_cryptohash_update(ctx, buff, len) < 0 ||
		pg_cryptohash_final(ctx, outbuf, MD5_DIGEST_LENGTH) < 0)
	{
		*errstr = pg_cryptohash_error(ctx);
		pg_cryptohash_free(ctx);
		return false;
	}

	pg_cryptohash_free(ctx);
	return true;
}


/*
 * Computes MD5 checksum of "passwd" (a null-terminated string) followed
 * by "salt" (which need not be null-terminated).
 *
 * Output format is "md5" followed by a 32-hex-digit MD5 checksum.
 * Hence, the output buffer "buf" must be at least 36 bytes long.
 *
 * Returns true if okay, false on error with *errstr providing some
 * error context.
 */
bool
pg_md5_encrypt(const char *passwd, const char *salt, size_t salt_len,
			   char *buf, const char **errstr)
{
	size_t		passwd_len = strlen(passwd);

	/* +1 here is just to avoid risk of unportable malloc(0) */
	char	   *crypt_buf = malloc(passwd_len + salt_len + 1);
	bool		ret;

	if (!crypt_buf)
	{
		*errstr = _("out of memory");
		return false;
	}

	/*
	 * Place salt at the end because it may be known by users trying to crack
	 * the MD5 output.
	 */
	memcpy(crypt_buf, passwd, passwd_len);
	memcpy(crypt_buf + passwd_len, salt, salt_len);

	strcpy(buf, "md5");
	ret = pg_md5_hash(crypt_buf, passwd_len + salt_len, buf + 3, errstr);

	free(crypt_buf);

	return ret;
}

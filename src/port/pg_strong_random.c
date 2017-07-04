/*-------------------------------------------------------------------------
 *
 * pg_strong_random.c
 *	  generate a cryptographically secure random number
 *
 * Our definition of "strong" is that it's suitable for generating random
 * salts and query cancellation keys, during authentication.
 *
 * Copyright (c) 1996-2017, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/pg_strong_random.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>

#ifdef USE_OPENSSL
#include <openssl/rand.h>
#endif
#ifdef WIN32
#include <wincrypt.h>
#endif

#ifdef WIN32
/*
 * Cache a global crypto provider that only gets freed when the process
 * exits, in case we need random numbers more than once.
 */
static HCRYPTPROV hProvider = 0;
#endif

#if defined(USE_DEV_URANDOM)
/*
 * Read (random) bytes from a file.
 */
static bool
random_from_file(char *filename, void *buf, size_t len)
{
	int			f;
	char	   *p = buf;
	ssize_t		res;

	f = open(filename, O_RDONLY, 0);
	if (f == -1)
		return false;

	while (len)
	{
		res = read(f, p, len);
		if (res <= 0)
		{
			if (errno == EINTR)
				continue;		/* interrupted by signal, just retry */

			close(f);
			return false;
		}

		p += res;
		len -= res;
	}

	close(f);
	return true;
}
#endif

/*
 * pg_strong_random
 *
 * Generate requested number of random bytes. The returned bytes are
 * cryptographically secure, suitable for use e.g. in authentication.
 *
 * We rely on system facilities for actually generating the numbers.
 * We support a number of sources:
 *
 * 1. OpenSSL's RAND_bytes()
 * 2. Windows' CryptGenRandom() function
 * 3. /dev/urandom
 *
 * The configure script will choose which one to use, and set
 * a USE_*_RANDOM flag accordingly.
 *
 * Returns true on success, and false if none of the sources
 * were available. NB: It is important to check the return value!
 * Proceeding with key generation when no random data was available
 * would lead to predictable keys and security issues.
 */
bool
pg_strong_random(void *buf, size_t len)
{
	/*
	 * When built with OpenSSL, use OpenSSL's RAND_bytes function.
	 */
#if defined(USE_OPENSSL_RANDOM)
	if (RAND_bytes(buf, len) == 1)
		return true;
	return false;

	/*
	 * Windows has CryptoAPI for strong cryptographic numbers.
	 */
#elif defined(USE_WIN32_RANDOM)
	if (hProvider == 0)
	{
		if (!CryptAcquireContext(&hProvider,
								 NULL,
								 MS_DEF_PROV,
								 PROV_RSA_FULL,
								 CRYPT_VERIFYCONTEXT | CRYPT_SILENT))
		{
			/*
			 * On failure, set back to 0 in case the value was for some reason
			 * modified.
			 */
			hProvider = 0;
		}
	}
	/* Re-check in case we just retrieved the provider */
	if (hProvider != 0)
	{
		if (CryptGenRandom(hProvider, len, buf))
			return true;
	}
	return false;

	/*
	 * Read /dev/urandom ourselves.
	 */
#elif defined(USE_DEV_URANDOM)
	if (random_from_file("/dev/urandom", buf, len))
		return true;
	return false;

#else
	/* The autoconf script should not have allowed this */
#error no source of random numbers configured
#endif
}

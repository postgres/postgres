/*-------------------------------------------------------------------------
 *
 * pg_strong_random.c
 *	  generate a cryptographically secure random number
 *
 * Our definition of "strong" is that it's suitable for generating random
 * salts and query cancellation keys, during authentication.
 *
 * Note: this code is run quite early in postmaster and backend startup;
 * therefore, even when built for backend, it cannot rely on backend
 * infrastructure such as elog() or palloc().
 *
 * Copyright (c) 1996-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/pg_strong_random.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>

#ifdef USE_OPENSSL
#include <openssl/rand.h>
#endif
#ifdef USE_WIN32_RANDOM
#include <wincrypt.h>
#endif

#ifdef USE_WIN32_RANDOM
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
random_from_file(const char *filename, void *buf, size_t len)
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
	int			i;

	/*
	 * Check that OpenSSL's CSPRNG has been sufficiently seeded, and if not
	 * add more seed data using RAND_poll().  With some older versions of
	 * OpenSSL, it may be necessary to call RAND_poll() a number of times.
	 */
#define NUM_RAND_POLL_RETRIES 8

	for (i = 0; i < NUM_RAND_POLL_RETRIES; i++)
	{
		if (RAND_status() == 1)
		{
			/* The CSPRNG is sufficiently seeded */
			break;
		}

		if (RAND_poll() == 0)
		{
			/*
			 * RAND_poll() failed to generate any seed data, which means that
			 * RAND_bytes() will probably fail.  For now, just fall through
			 * and let that happen.  XXX: maybe we could seed it some other
			 * way.
			 */
			break;
		}
	}

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

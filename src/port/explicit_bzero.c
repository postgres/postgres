/*-------------------------------------------------------------------------
 *
 * explicit_bzero.c
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/explicit_bzero.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#if defined(HAVE_MEMSET_S)

void
explicit_bzero(void *buf, size_t len)
{
	(void) memset_s(buf, len, 0, len);
}

#elif defined(WIN32)

void
explicit_bzero(void *buf, size_t len)
{
	(void) SecureZeroMemory(buf, len);
}

#else

/*
 * Indirect call through a volatile pointer to hopefully avoid dead-store
 * optimisation eliminating the call.  (Idea taken from OpenSSH.)  We can't
 * assume bzero() is present either, so for simplicity we define our own.
 */

static void
bzero2(void *buf, size_t len)
{
	memset(buf, 0, len);
}

static void (*volatile bzero_p) (void *, size_t) = bzero2;

void
explicit_bzero(void *buf, size_t len)
{
	bzero_p(buf, len);
}

#endif

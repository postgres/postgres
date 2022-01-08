/*-------------------------------------------------------------------------
 *
 * pgstrsignal.c
 *	  Identify a Unix signal number
 *
 * On platforms compliant with modern POSIX, this just wraps strsignal(3).
 * Elsewhere, we do the best we can.
 *
 * This file is not currently built in MSVC builds, since it's useless
 * on non-Unix platforms.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/port/pgstrsignal.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"


/*
 * pg_strsignal
 *
 * Return a string identifying the given Unix signal number.
 *
 * The result is declared "const char *" because callers should not
 * modify the string.  Note, however, that POSIX does not promise that
 * the string will remain valid across later calls to strsignal().
 *
 * This version guarantees to return a non-NULL pointer, although
 * some platforms' versions of strsignal() reputedly do not.
 *
 * Note that the fallback cases just return constant strings such as
 * "unrecognized signal".  Project style is for callers to print the
 * numeric signal value along with the result of this function, so
 * there's no need to work harder than that.
 */
const char *
pg_strsignal(int signum)
{
	const char *result;

	/*
	 * If we have strsignal(3), use that --- but check its result for NULL.
	 */
#ifdef HAVE_STRSIGNAL
	result = strsignal(signum);
	if (result == NULL)
		result = "unrecognized signal";
#else

	/*
	 * We used to have code here to try to use sys_siglist[] if available.
	 * However, it seems that all platforms with sys_siglist[] have also had
	 * strsignal() for many years now, so that was just a waste of code.
	 */
	result = "(signal names not available on this platform)";
#endif

	return result;
}

/*-------------------------------------------------------------------------
 *
 * psprintf.c
 *		sprintf into an allocated-on-demand buffer
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/common/psprintf.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "utils/memutils.h"


static size_t pvsnprintf(char *buf, size_t len, const char *fmt, va_list args)
__attribute__((format(PG_PRINTF_ATTRIBUTE, 3, 0)));


/*
 * psprintf
 *
 * Format text data under the control of fmt (an sprintf-style format string)
 * and return it in an allocated-on-demand buffer.	The buffer is allocated
 * with palloc in the backend, or malloc in frontend builds.  Caller is
 * responsible to free the buffer when no longer needed, if appropriate.
 *
 * Errors are not returned to the caller, but are reported via elog(ERROR)
 * in the backend, or printf-to-stderr-and-exit() in frontend builds.
 * One should therefore think twice about using this in libpq.
 */
char *
psprintf(const char *fmt,...)
{
	size_t		len = 128;		/* initial assumption about buffer size */

	for (;;)
	{
		char	   *result;
		va_list		args;

		/*
		 * Allocate result buffer.	Note that in frontend this maps to malloc
		 * with exit-on-error.
		 */
		result = (char *) palloc(len);

		/* Try to format the data. */
		va_start(args, fmt);
		len = pvsnprintf(result, len, fmt, args);
		va_end(args);

		if (len == 0)
			return result;		/* success */

		/* Release buffer and loop around to try again with larger len. */
		pfree(result);
	}
}

/*
 * pvsnprintf
 *
 * Attempt to format text data under the control of fmt (an sprintf-style
 * format string) and insert it into buf (which has length len).
 *
 * If successful, return zero.	If there's not enough space in buf, return
 * an estimate of the buffer size needed to succeed (this *must* be more
 * than "len", else psprintf might loop infinitely).
 * Other error cases do not return.
 *
 * XXX This API is ugly, but there seems no alternative given the C spec's
 * restrictions on what can portably be done with va_list arguments: you have
 * to redo va_start before you can rescan the argument list, and we can't do
 * that from here.
 */
static size_t
pvsnprintf(char *buf, size_t len, const char *fmt, va_list args)
{
	int			nprinted;

	Assert(len > 0);

	errno = 0;

	/*
	 * Assert check here is to catch buggy vsnprintf that overruns the
	 * specified buffer length.  Solaris 7 in 64-bit mode is an example of a
	 * platform with such a bug.
	 */
#ifdef USE_ASSERT_CHECKING
	buf[len - 1] = '\0';
#endif

	nprinted = vsnprintf(buf, len, fmt, args);

	Assert(buf[len - 1] == '\0');

	/*
	 * If vsnprintf reports an error other than ENOMEM, fail.  The possible
	 * causes of this are not user-facing errors, so elog should be enough.
	 */
	if (nprinted < 0 && errno != 0 && errno != ENOMEM)
	{
#ifndef FRONTEND
		elog(ERROR, "vsnprintf failed: %m");
#else
		fprintf(stderr, "vsnprintf failed: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
#endif
	}

	/*
	 * Note: some versions of vsnprintf return the number of chars actually
	 * stored, not the total space needed as C99 specifies.  And at least one
	 * returns -1 on failure.  Be conservative about believing whether the
	 * print worked.
	 */
	if (nprinted >= 0 && (size_t) nprinted < len - 1)
	{
		/* Success.  Note nprinted does not include trailing null. */
		return 0;
	}

	if (nprinted >= 0 && (size_t) nprinted > len)
	{
		/*
		 * This appears to be a C99-compliant vsnprintf, so believe its
		 * estimate of the required space.	(If it's wrong, this code will
		 * still work, but may loop multiple times.)  Note that the space
		 * needed should be only nprinted+1 bytes, but we'd better allocate
		 * one more than that so that the test above will succeed next time.
		 *
		 * In the corner case where the required space just barely overflows,
		 * fall through so that we'll error out below (possibly after looping).
		 */
		if ((size_t) nprinted <= MaxAllocSize - 2)
			return nprinted + 2;
	}

	/*
	 * Buffer overrun, and we don't know how much space is needed.  Estimate
	 * twice the previous buffer size.	If this would overflow, choke.	We use
	 * a palloc-oriented overflow limit even when in frontend.
	 */
	if (len > MaxAllocSize / 2)
	{
#ifndef FRONTEND
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
#else
		fprintf(stderr, _("out of memory\n"));
		exit(EXIT_FAILURE);
#endif
	}

	return len * 2;
}


/*
 * XXX this is going away shortly.
 */
#ifdef FRONTEND
int
pg_asprintf(char **ret, const char *fmt, ...)
{
	size_t		len = 128;		/* initial assumption about buffer size */

	for (;;)
	{
		char	   *result;
		va_list		args;

		/*
		 * Allocate result buffer.	Note that in frontend this maps to malloc
		 * with exit-on-error.
		 */
		result = (char *) palloc(len);

		/* Try to format the data. */
		va_start(args, fmt);
		len = pvsnprintf(result, len, fmt, args);
		va_end(args);

		if (len == 0)
		{
			*ret = result;
			return 0;
		}

		/* Release buffer and loop around to try again with larger len. */
		pfree(result);
	}
}
#endif

/*-------------------------------------------------------------------------
 *
 * threads.c
 *
 *		  Prototypes and macros around system calls, used to help make
 *		  threaded libraries reentrant and safe to use from threaded applications.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 * $Id: thread.c,v 1.1 2003/08/08 02:46:40 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

/*
 * Wrapper around strerror and strerror_r to use the former if it is
 * available and also return a more useful value (the error string).
 */
char *
pqStrerror(int errnum, char *strerrbuf, size_t buflen)
{
#if defined(USE_THREADS) && defined(HAVE_STRERROR_R)
	/* reentrant strerror_r is available */
	/* some early standards had strerror_r returning char * */
	strerror_r(errnum, strerrbuf, buflen);
	return (strerrbuf);
#else
	/* no strerror_r() available, just use strerror */
	return strerror(errnum);
#endif
}

/*
 * Wrapper around getpwuid() or getpwuid_r() to mimic POSIX getpwuid_r()
 * behaviour, if it is not available.
 */
int
pqGetpwuid(uid_t uid, struct passwd * resultbuf, char *buffer,
		   size_t buflen, struct passwd ** result)
{
#if defined(USE_THREADS) && defined(HAVE_GETPWUID_R)

	/*
	 * broken (well early POSIX draft) getpwuid_r() which returns 'struct
	 * passwd *'
	 */
	*result = getpwuid_r(uid, resultbuf, buffer, buflen);
#else
	/* no getpwuid_r() available, just use getpwuid() */
	*result = getpwuid(uid);
#endif
	return (*result == NULL) ? -1 : 0;
}

/*
 * Wrapper around gethostbyname() or gethostbyname_r() to mimic
 * POSIX gethostbyname_r() behaviour, if it is not available.
 */
int
pqGethostbyname(const char *name,
				struct hostent * resbuf,
				char *buf, size_t buflen,
				struct hostent ** result,
				int *herrno)
{
#if defined(USE_THREADS) && defined(HAVE_GETHOSTBYNAME_R)

	/*
	 * broken (well early POSIX draft) gethostbyname_r() which returns
	 * 'struct hostent *'
	 */
	*result = gethostbyname_r(name, resbuf, buf, buflen, herrno);
	return (*result == NULL) ? -1 : 0;
#else
	/* no gethostbyname_r(), just use gethostbyname() */
	*result = gethostbyname(name);
	if (*result != NULL)
		return 0;
	else
	{
		*herrno = h_errno;
		return -1;
	}
#endif
}

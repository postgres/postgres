/*-------------------------------------------------------------------------
 *
 * thread.c
 *
 *		  Prototypes and macros around system calls, used to help make
 *		  threaded libraries reentrant and safe to use from threaded applications.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 * $Id: thread.c,v 1.6 2003/09/05 17:43:40 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

/*
 *	Threading sometimes requires specially-named versions of functions
 *	that return data in static buffers, like strerror_r() instead of
 *	strerror().  Other operating systems use pthread_setspecific()
 *	and pthread_getspecific() internally to allow standard library
 *	functions to return static data to threaded applications.
 *
 *	Additional confusion exists because many operating systems that
 *	use pthread_setspecific/pthread_getspecific() also have *_r versions
 *	of standard library functions for compatibility with operating systems
 *	that require them.  However, internally, these *_r functions merely
 *	call the thread-safe standard library functions.
 *
 *	For example, BSD/OS 4.3 uses Bind 8.2.3 for getpwuid().  Internally,
 *	getpwuid() calls pthread_setspecific/pthread_getspecific() to return
 *	static data to the caller in a thread-safe manner.  However, BSD/OS
 *	also has getpwuid_r(), which merely calls getpwuid() and shifts
 *	around the arguments to match the getpwuid_r() function declaration.
 *	Therefore, while BSD/OS has getpwuid_r(), it isn't required.  It also
 *	doesn't have strerror_r(), so we can't fall back to only using *_r
 *	functions for threaded programs.
 *
 *	The current setup is to assume either all standard functions are
 *	thread-safe (NEED_REENTRANT_FUNC_NAMES=no), or the operating system
 *	requires reentrant function names (NEED_REENTRANT_FUNC_NAMES=yes).
 *	Compile and run src/tools/test_thread_funcs.c to see if your operating
 *	system requires reentrant function names.
 */
 

/*
 * Wrapper around strerror and strerror_r to use the former if it is
 * available and also return a more useful value (the error string).
 */
char *
pqStrerror(int errnum, char *strerrbuf, size_t buflen)
{
#if defined(USE_THREADS) && defined(NEED_REENTRANT_FUNC_NAMES)
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
 * behaviour, if it is not available or required.
 */
#ifndef WIN32
int
pqGetpwuid(uid_t uid, struct passwd *resultbuf, char *buffer,
		   size_t buflen, struct passwd **result)
{
#if defined(USE_THREADS) && defined(NEED_REENTRANT_FUNC_NAMES)
	/*
	 * Early POSIX draft of getpwuid_r() returns 'struct passwd *'.
	 *    getpwuid_r(uid, resultbuf, buffer, buflen)
	 * Do we need to support it?  bjm 2003-08-14
	 */
	/* POSIX version */
	getpwuid_r(uid, resultbuf, buffer, buflen, result);
#else
	/* no getpwuid_r() available, just use getpwuid() */
	*result = getpwuid(uid);
#endif
	return (*result == NULL) ? -1 : 0;
}
#endif

/*
 * Wrapper around gethostbyname() or gethostbyname_r() to mimic
 * POSIX gethostbyname_r() behaviour, if it is not available or required.
 */
int
pqGethostbyname(const char *name,
				struct hostent *resbuf,
				char *buf, size_t buflen,
				struct hostent **result,
				int *herrno)
{
#if defined(USE_THREADS) && defined(NEED_REENTRANT_FUNC_NAMES)
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

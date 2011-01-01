/*-------------------------------------------------------------------------
 *
 * thread.c
 *
 *		  Prototypes and macros around system calls, used to help make
 *		  threaded libraries reentrant and safe to use from threaded applications.
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 *
 * src/port/thread.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <pwd.h>


/*
 *	Threading sometimes requires specially-named versions of functions
 *	that return data in static buffers, like strerror_r() instead of
 *	strerror().  Other operating systems use pthread_setspecific()
 *	and pthread_getspecific() internally to allow standard library
 *	functions to return static data to threaded applications. And some
 *	operating systems have neither.
 *
 *	Additional confusion exists because many operating systems that
 *	use pthread_setspecific/pthread_getspecific() also have *_r versions
 *	of standard library functions for compatibility with operating systems
 *	that require them.	However, internally, these *_r functions merely
 *	call the thread-safe standard library functions.
 *
 *	For example, BSD/OS 4.3 uses Bind 8.2.3 for getpwuid().  Internally,
 *	getpwuid() calls pthread_setspecific/pthread_getspecific() to return
 *	static data to the caller in a thread-safe manner.	However, BSD/OS
 *	also has getpwuid_r(), which merely calls getpwuid() and shifts
 *	around the arguments to match the getpwuid_r() function declaration.
 *	Therefore, while BSD/OS has getpwuid_r(), it isn't required.  It also
 *	doesn't have strerror_r(), so we can't fall back to only using *_r
 *	functions for threaded programs.
 *
 *	The current setup is to try threading in this order:
 *
 *		use *_r function names if they exit
 *			(*_THREADSAFE=yes)
 *		use non-*_r functions if they are thread-safe
 *
 *	One thread-safe solution for gethostbyname() might be to use getaddrinfo().
 *
 *	Run src/test/thread to test if your operating system has thread-safe
 *	non-*_r functions.
 */


/*
 * Wrapper around strerror and strerror_r to use the former if it is
 * available and also return a more useful value (the error string).
 */
char *
pqStrerror(int errnum, char *strerrbuf, size_t buflen)
{
#if defined(FRONTEND) && defined(ENABLE_THREAD_SAFETY) && defined(HAVE_STRERROR_R)
	/* reentrant strerror_r is available */
#ifdef STRERROR_R_INT
	/* SUSv3 version */
	if (strerror_r(errnum, strerrbuf, buflen) == 0)
		return strerrbuf;
	else
		return "Unknown error";
#else
	/* GNU libc */
	return strerror_r(errnum, strerrbuf, buflen);
#endif
#else
	/* no strerror_r() available, just use strerror */
	strlcpy(strerrbuf, strerror(errnum), buflen);

	return strerrbuf;
#endif
}

/*
 * Wrapper around getpwuid() or getpwuid_r() to mimic POSIX getpwuid_r()
 * behaviour, if it is not available or required.
 */
#ifndef WIN32
int
pqGetpwuid(uid_t uid, struct passwd * resultbuf, char *buffer,
		   size_t buflen, struct passwd ** result)
{
#if defined(FRONTEND) && defined(ENABLE_THREAD_SAFETY) && defined(HAVE_GETPWUID_R)

#ifdef GETPWUID_R_5ARG
	/* POSIX version */
	getpwuid_r(uid, resultbuf, buffer, buflen, result);
#else

	/*
	 * Early POSIX draft of getpwuid_r() returns 'struct passwd *'.
	 * getpwuid_r(uid, resultbuf, buffer, buflen)
	 */
	*result = getpwuid_r(uid, resultbuf, buffer, buflen);
#endif
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
 * This function is called _only_ by our getaddinfo() portability function.
 */
#ifndef HAVE_GETADDRINFO
int
pqGethostbyname(const char *name,
				struct hostent * resultbuf,
				char *buffer, size_t buflen,
				struct hostent ** result,
				int *herrno)
{
#if defined(FRONTEND) && defined(ENABLE_THREAD_SAFETY) && defined(HAVE_GETHOSTBYNAME_R)

	/*
	 * broken (well early POSIX draft) gethostbyname_r() which returns 'struct
	 * hostent *'
	 */
	*result = gethostbyname_r(name, resultbuf, buffer, buflen, herrno);
	return (*result == NULL) ? -1 : 0;
#else

	/* no gethostbyname_r(), just use gethostbyname() */
	*result = gethostbyname(name);

	if (*result != NULL)
		*herrno = h_errno;

	if (*result != NULL)
		return 0;
	else
		return -1;
#endif
}

#endif

/*-------------------------------------------------------------------------
 *
 * thread.c
 *
 *		  Prototypes and macros around system calls, used to help make
 *		  threaded libraries reentrant and safe to use from threaded applications.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 * $Id: thread.c,v 1.12.2.5 2004/03/23 02:04:33 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/types.h>
#include <errno.h>
#if defined(WIN32) && (defined(_MSC_VER) || defined(__BORLANDC__))
#undef ERROR
#else
#include <pwd.h>
#endif
#if defined(ENABLE_THREAD_SAFETY)
#include <pthread.h>
#endif

/*
 *	Threading sometimes requires specially-named versions of functions
 *	that return data in static buffers, like strerror_r() instead of
 *	strerror().  Other operating systems use pthread_setspecific()
 *	and pthread_getspecific() internally to allow standard library
 *	functions to return static data to threaded applications. And some
 *	operating systems have neither, meaning we have to do our own locking.
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
 *	The current setup is to try threading in this order:
 *
 *		use non-*_r function names if they are all thread-safe
 *			(NEED_REENTRANT_FUNCS=no)
 *		use *_r functions if they exist (configure test)
 *		do our own locking and copying of non-threadsafe functions
 *
 *	The disadvantage of the last option is not the thread overhead but
 *	the fact that all function calls are serialized, and with gethostbyname()
 *	requiring a DNS lookup, that could be slow.
 *
 *	One thread-safe solution for gethostbyname() might be to use getaddrinfo().
 *
 *	See src/tools/thread to see if your operating system has thread-safe
 *	non-*_r functions.
 */
 

/*
 * Wrapper around strerror and strerror_r to use the former if it is
 * available and also return a more useful value (the error string).
 */
char *
pqStrerror(int errnum, char *strerrbuf, size_t buflen)
{
#if defined(FRONTEND) && defined(ENABLE_THREAD_SAFETY) && defined(NEED_REENTRANT_FUNCS) && defined(HAVE_STRERROR_R)
	/* reentrant strerror_r is available */
	/* some early standards had strerror_r returning char * */
	strerror_r(errnum, strerrbuf, buflen);
	return strerrbuf;

#else

#if defined(FRONTEND) && defined(ENABLE_THREAD_SAFETY) && defined(NEED_REENTRANT_FUNCS) && !defined(HAVE_STRERROR_R)
	static pthread_mutex_t strerror_lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_lock(&strerror_lock);
#endif

	/* no strerror_r() available, just use strerror */
	StrNCpy(strerrbuf, strerror(errnum), buflen);

#if defined(FRONTEND) && defined(ENABLE_THREAD_SAFETY) && defined(NEED_REENTRANT_FUNCS) && !defined(HAVE_STRERROR_R)
	pthread_mutex_unlock(&strerror_lock);
#endif

	return strerrbuf;
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
#if defined(FRONTEND) && defined(ENABLE_THREAD_SAFETY) && defined(NEED_REENTRANT_FUNCS) && defined(HAVE_GETPWUID_R)

#ifdef GETPWUID_R_5ARG
	/* POSIX version */
	getpwuid_r(uid, resultbuf, buffer, buflen, result);
#else
	/*
	 * Early POSIX draft of getpwuid_r() returns 'struct passwd *'.
	 *    getpwuid_r(uid, resultbuf, buffer, buflen)
	 */
	*result = getpwuid_r(uid, resultbuf, buffer, buflen);
#endif
#else

#if defined(FRONTEND) && defined(ENABLE_THREAD_SAFETY) && defined(NEED_REENTRANT_FUNCS) && !defined(HAVE_GETPWUID_R)
	static pthread_mutex_t getpwuid_lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_lock(&getpwuid_lock);
#endif

	/* no getpwuid_r() available, just use getpwuid() */
	*result = getpwuid(uid);

#if defined(FRONTEND) && defined(ENABLE_THREAD_SAFETY) && defined(NEED_REENTRANT_FUNCS) && !defined(HAVE_GETPWUID_R)

	/* Use 'buffer' memory for storage of strings used by struct passwd */
	if (*result &&
		strlen((*result)->pw_name) + 1 +
		strlen((*result)->pw_passwd) + 1 +
		strlen((*result)->pw_gecos) + 1 +
		/* skip class if it exists */
		strlen((*result)->pw_dir) + 1 +
		strlen((*result)->pw_shell) + 1 <= buflen)
	{
		memcpy(resultbuf, *result, sizeof(struct passwd));
		strcpy(buffer, (*result)->pw_name);
		resultbuf->pw_name = buffer;
		buffer += strlen(resultbuf->pw_name) + 1;
		strcpy(buffer, (*result)->pw_passwd);
		resultbuf->pw_passwd = buffer;
		buffer += strlen(resultbuf->pw_passwd) + 1;
		strcpy(buffer, (*result)->pw_gecos);
		resultbuf->pw_gecos = buffer;
		buffer += strlen(resultbuf->pw_gecos) + 1;
		strcpy(buffer, (*result)->pw_dir);
		resultbuf->pw_dir = buffer;
		buffer += strlen(resultbuf->pw_dir) + 1;
		strcpy(buffer, (*result)->pw_shell);
		resultbuf->pw_shell = buffer;
		buffer += strlen(resultbuf->pw_shell) + 1;

		*result = resultbuf;
	}
	else
	{
		*result = NULL;
		errno = ERANGE;
	}

	pthread_mutex_unlock(&getpwuid_lock);
#endif
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
				struct hostent *resultbuf,
				char *buffer, size_t buflen,
				struct hostent **result,
				int *herrno)
{
#if defined(FRONTEND) && defined(ENABLE_THREAD_SAFETY) && defined(NEED_REENTRANT_FUNCS) && defined(HAVE_GETHOSTBYNAME_R)
	/*
	 * broken (well early POSIX draft) gethostbyname_r() which returns
	 * 'struct hostent *'
	 */
	*result = gethostbyname_r(name, resultbuf, buffer, buflen, herrno);
	return (*result == NULL) ? -1 : 0;

#else

#if defined(FRONTEND) && defined(ENABLE_THREAD_SAFETY) && defined(NEED_REENTRANT_FUNCS) && !defined(HAVE_GETHOSTBYNAME_R)
	static pthread_mutex_t gethostbyname_lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_lock(&gethostbyname_lock);
#endif

	/* no gethostbyname_r(), just use gethostbyname() */
	*result = gethostbyname(name);

#if defined(FRONTEND) && defined(ENABLE_THREAD_SAFETY) && defined(NEED_REENTRANT_FUNCS) && !defined(HAVE_GETHOSTBYNAME_R)

	/*
	 *	Use 'buffer' memory for storage of structures used by struct hostent.
	 *	The layout is:
	 *
	 *		addr pointers
	 *		alias pointers
	 *		addr structures
	 *		alias structures
	 *		name
	 */
	if (*result)
	{
		int		i, pointers = 2 /* for nulls */, len = 0;
		char	**pbuffer;

		for (i = 0; (*result)->h_addr_list[i]; i++, pointers++)
			len += (*result)->h_length;
		for (i = 0; (*result)->h_aliases[i]; i++, pointers++)
			len += (*result)->h_length;

		if (pointers * sizeof(char *) + MAXALIGN(len) + strlen((*result)->h_name) + 1 <= buflen)
		{
			memcpy(resultbuf, *result, sizeof(struct hostent));

    		pbuffer = (char **)buffer;
    		resultbuf->h_addr_list = pbuffer;
    		buffer += pointers * sizeof(char *);

			for (i = 0; (*result)->h_addr_list[i]; i++, pbuffer++)
			{
				memcpy(buffer, (*result)->h_addr_list[i], (*result)->h_length);
    			resultbuf->h_addr_list[i] = buffer;
    			buffer += (*result)->h_length;
    		}
			resultbuf->h_addr_list[i] = NULL;
			pbuffer++;
			    
    		resultbuf->h_aliases = pbuffer;

			for (i = 0; (*result)->h_aliases[i]; i++, pbuffer++)
			{
				memcpy(buffer, (*result)->h_aliases[i], (*result)->h_length);
    			resultbuf->h_aliases[i] = buffer;
    			buffer += (*result)->h_length;
    		}
			resultbuf->h_aliases[i] = NULL;
			pbuffer++;

			/* Place at end for cleaner alignment */			
			buffer = MAXALIGN(buffer);
			strcpy(buffer, (*result)->h_name);
			resultbuf->h_name = buffer;
			buffer += strlen(resultbuf->h_name) + 1;

			*result = resultbuf;
		}
		else
		{
			*result = NULL;
			errno = ERANGE;
		}
	}
#endif

	if (*result != NULL)
		*herrno = h_errno;
		
#if defined(FRONTEND) && defined(ENABLE_THREAD_SAFETY) && defined(NEED_REENTRANT_FUNCS) && !defined(HAVE_GETHOSTBYNAME_R)
	pthread_mutex_unlock(&gethostbyname_lock);
#endif

	if (*result != NULL)
		return 0;
	else
		return -1;
#endif
}
#endif

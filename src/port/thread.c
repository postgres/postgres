/*-------------------------------------------------------------------------
 *
 * thread.c
 *
 *		  Prototypes and macros around system calls, used to help make
 *		  threaded libraries reentrant and safe to use from threaded applications.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
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
 *		use *_r function names if they exit
 *			(*_THREADSAFE=yes)
 *		use non-*_r functions if they are thread-safe
 */

#ifndef WIN32

/*
 * Wrapper around getpwuid() or getpwuid_r() to mimic POSIX getpwuid_r()
 * behaviour, if that function is not available or required.
 *
 * Per POSIX, the possible cases are:
 * success: returns zero, *result is non-NULL
 * uid not found: returns zero, *result is NULL
 * error during lookup: returns an errno code, *result is NULL
 * (caller should *not* assume that the errno variable is set)
 */
static int
pqGetpwuid(uid_t uid, struct passwd *resultbuf, char *buffer,
		   size_t buflen, struct passwd **result)
{
#if defined(FRONTEND) && defined(ENABLE_THREAD_SAFETY) && defined(HAVE_GETPWUID_R)
	return getpwuid_r(uid, resultbuf, buffer, buflen, result);
#else
	/* no getpwuid_r() available, just use getpwuid() */
	errno = 0;
	*result = getpwuid(uid);
	/* paranoia: ensure we return zero on success */
	return (*result == NULL) ? errno : 0;
#endif
}

/*
 * pg_get_user_name - get the name of the user with the given ID
 *
 * On success, the user name is returned into the buffer (of size buflen),
 * and "true" is returned.  On failure, a localized error message is
 * returned into the buffer, and "false" is returned.
 */
bool
pg_get_user_name(uid_t user_id, char *buffer, size_t buflen)
{
	char		pwdbuf[BUFSIZ];
	struct passwd pwdstr;
	struct passwd *pw = NULL;
	int			pwerr;

	pwerr = pqGetpwuid(user_id, &pwdstr, pwdbuf, sizeof(pwdbuf), &pw);
	if (pw != NULL)
	{
		strlcpy(buffer, pw->pw_name, buflen);
		return true;
	}
	if (pwerr != 0)
		snprintf(buffer, buflen,
				 _("could not look up local user ID %d: %s"),
				 (int) user_id,
				 strerror_r(pwerr, pwdbuf, sizeof(pwdbuf)));
	else
		snprintf(buffer, buflen,
				 _("local user with ID %d does not exist"),
				 (int) user_id);
	return false;
}

/*
 * pg_get_user_home_dir - get the home directory of the user with the given ID
 *
 * On success, the directory path is returned into the buffer (of size buflen),
 * and "true" is returned.  On failure, a localized error message is
 * returned into the buffer, and "false" is returned.
 *
 * Note that this does not incorporate the common behavior of checking
 * $HOME first, since it's independent of which user_id is queried.
 */
bool
pg_get_user_home_dir(uid_t user_id, char *buffer, size_t buflen)
{
	char		pwdbuf[BUFSIZ];
	struct passwd pwdstr;
	struct passwd *pw = NULL;
	int			pwerr;

	pwerr = pqGetpwuid(user_id, &pwdstr, pwdbuf, sizeof(pwdbuf), &pw);
	if (pw != NULL)
	{
		strlcpy(buffer, pw->pw_dir, buflen);
		return true;
	}
	if (pwerr != 0)
		snprintf(buffer, buflen,
				 _("could not look up local user ID %d: %s"),
				 (int) user_id,
				 strerror_r(pwerr, pwdbuf, sizeof(pwdbuf)));
	else
		snprintf(buffer, buflen,
				 _("local user with ID %d does not exist"),
				 (int) user_id);
	return false;
}

#endif							/* !WIN32 */

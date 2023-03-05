/*-------------------------------------------------------------------------
 *
 * thread.c
 *
 *		  Prototypes and macros around system calls, used to help make
 *		  threaded libraries reentrant and safe to use from threaded applications.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * src/port/thread.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <pwd.h>


/*
 *  Historically, the code in this module had to deal with operating systems
 *  that lacked getpwuid_r().
 */

#ifndef WIN32

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

	pwerr = getpwuid_r(user_id, &pwdstr, pwdbuf, sizeof(pwdbuf), &pw);
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

	pwerr = getpwuid_r(user_id, &pwdstr, pwdbuf, sizeof(pwdbuf), &pw);
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

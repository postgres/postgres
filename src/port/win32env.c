/*-------------------------------------------------------------------------
 *
 * win32env.c
 *	  putenv(), setenv(), and unsetenv() for win32.
 *
 * These functions update both the process environment and caches in
 * (potentially multiple) C run-time library (CRT) versions.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/win32env.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"


/*
 * Note that unlike POSIX putenv(), this doesn't use the passed-in string
 * as permanent storage.
 */
int
pgwin32_putenv(const char *envval)
{
	char	   *envcpy;
	char	   *cp;
	typedef int (_cdecl * PUTENVPROC) (const char *);
	static const char *const modulenames[] = {
		"msvcrt",				/* Visual Studio 6.0 / MinGW */
		"msvcrtd",
		"msvcr70",				/* Visual Studio 2002 */
		"msvcr70d",
		"msvcr71",				/* Visual Studio 2003 */
		"msvcr71d",
		"msvcr80",				/* Visual Studio 2005 */
		"msvcr80d",
		"msvcr90",				/* Visual Studio 2008 */
		"msvcr90d",
		"msvcr100",				/* Visual Studio 2010 */
		"msvcr100d",
		"msvcr110",				/* Visual Studio 2012 */
		"msvcr110d",
		"msvcr120",				/* Visual Studio 2013 */
		"msvcr120d",
		"ucrtbase",				/* Visual Studio 2015 and later */
		"ucrtbased",
		NULL
	};
	int			i;

	/*
	 * Update process environment, making this change visible to child
	 * processes and to CRTs initializing in the future.  Do this before the
	 * _putenv() loop, for the benefit of any CRT that initializes during this
	 * pgwin32_putenv() execution, after the loop checks that CRT.
	 *
	 * Need a copy of the string so we can modify it.
	 */
	envcpy = strdup(envval);
	if (!envcpy)
		return -1;
	cp = strchr(envcpy, '=');
	if (cp == NULL)
	{
		free(envcpy);
		return -1;
	}
	*cp = '\0';
	cp++;
	if (*cp)
	{
		/*
		 * Only call SetEnvironmentVariable() when we are adding a variable,
		 * not when removing it. Calling it on both crashes on at least
		 * certain versions of MinGW.
		 */
		if (!SetEnvironmentVariable(envcpy, cp))
		{
			free(envcpy);
			return -1;
		}
	}
	free(envcpy);

	/*
	 * Each CRT has its own _putenv() symbol and copy of the environment.
	 * Update the environment in each CRT module currently loaded, so every
	 * third-party library sees this change regardless of the CRT it links
	 * against.  Addresses within these modules may become invalid the moment
	 * we call FreeLibrary(), so don't cache them.
	 */
	for (i = 0; modulenames[i]; i++)
	{
		HMODULE		hmodule = NULL;
		BOOL		res = GetModuleHandleEx(0, modulenames[i], &hmodule);

		if (res != 0 && hmodule != NULL)
		{
			PUTENVPROC	putenvFunc;

			putenvFunc = (PUTENVPROC) (pg_funcptr_t) GetProcAddress(hmodule, "_putenv");
			if (putenvFunc)
				putenvFunc(envval);
			FreeLibrary(hmodule);
		}
	}

	/*
	 * Finally, update our "own" cache.  This is redundant with the loop
	 * above, except when PostgreSQL itself links to a CRT not listed above.
	 * Ideally, the loop does visit all possible CRTs, making this redundant.
	 */
	return _putenv(envval);
}

int
pgwin32_setenv(const char *name, const char *value, int overwrite)
{
	int			res;
	char	   *envstr;

	/* Error conditions, per POSIX */
	if (name == NULL || name[0] == '\0' || strchr(name, '=') != NULL ||
		value == NULL)
	{
		errno = EINVAL;
		return -1;
	}

	/* No work if variable exists and we're not to replace it */
	if (overwrite == 0 && getenv(name) != NULL)
		return 0;

	envstr = (char *) malloc(strlen(name) + strlen(value) + 2);
	if (!envstr)				/* not much we can do if no memory */
		return -1;

	sprintf(envstr, "%s=%s", name, value);

	res = pgwin32_putenv(envstr);
	free(envstr);
	return res;
}

int
pgwin32_unsetenv(const char *name)
{
	int			res;
	char	   *envbuf;

	envbuf = (char *) malloc(strlen(name) + 2);
	if (!envbuf)
		return -1;

	sprintf(envbuf, "%s=", name);
	res = pgwin32_putenv(envbuf);
	free(envbuf);
	return res;
}

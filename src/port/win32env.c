/*-------------------------------------------------------------------------
 *
 * win32env.c
 *	  putenv() and unsetenv() for win32, that updates both process
 *	  environment and the cached versions in (potentially multiple)
 *	  MSVCRT.
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/port/win32env.c,v 1.5 2009/12/27 16:11:28 mha Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

int
pgwin32_putenv(const char *envval)
{
	char	   *envcpy;
	char	   *cp;

	/*
	 * Each version of MSVCRT has its own _putenv() call in the runtime
	 * library.
	 *
	 * If we're in VC 7.0 or later (means != mingw), update in the 6.0
	 * MSVCRT.DLL environment as well, to work with third party libraries
	 * linked against it (such as gnuwin32 libraries).
	 */
#if defined(_MSC_VER) && (_MSC_VER >= 1300)
	typedef int (_cdecl * PUTENVPROC) (const char *);
	HMODULE		hmodule;
	static PUTENVPROC putenvFunc = NULL;

	if (putenvFunc == NULL)
	{
		hmodule = GetModuleHandle("msvcrt");
		if (hmodule != NULL)
		{
			/*
			 * If the module is found, attempt to find the function. If not, that just
			 * means we're not linked with msvcrt, so fall through and make our other
			 * modifications anyway.
			 * Ignore any errors and update whatever we can, since callers don't
			 * check the return value anyway.
			 */
			putenvFunc = (PUTENVPROC) GetProcAddress(hmodule, "_putenv");
			if (putenvFunc != NULL)
				putenvFunc(envval);
		}
	}
#endif   /* _MSC_VER >= 1300 */


	/*
	 * Update the process environment - to make modifications visible to child
	 * processes.
	 *
	 * Need a copy of the string so we can modify it.
	 */
	envcpy = strdup(envval);
	cp = strchr(envcpy, '=');
	if (cp == NULL)
		return -1;
	*cp = '\0';
	cp++;
	if (strlen(cp))
	{
		/*
		 * Only call SetEnvironmentVariable() when we are adding a variable,
		 * not when removing it. Calling it on both crashes on at least
		 * certain versions of MingW.
		 */
		if (!SetEnvironmentVariable(envcpy, cp))
		{
			free(envcpy);
			return -1;
		}
	}
	free(envcpy);

	/* Finally, update our "own" cache */
	return _putenv(envval);
}

void
pgwin32_unsetenv(const char *name)
{
	char	   *envbuf;

	envbuf = (char *) malloc(strlen(name) + 2);
	if (!envbuf)
		return;

	sprintf(envbuf, "%s=", name);
	pgwin32_putenv(envbuf);
	free(envbuf);
}

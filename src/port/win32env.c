/*-------------------------------------------------------------------------
 *
 * win32env.c
 *	  putenv() and unsetenv() for win32, which update both process environment
 *	  and caches in (potentially multiple) C run-time library (CRT) versions.
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/win32env.c
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
	 * Each CRT has its own _putenv() symbol and copy of the environment.
	 * Update the environment in each CRT module currently loaded, so every
	 * third-party library sees this change regardless of the CRT it links
	 * against.
	 */
#ifdef _MSC_VER
	typedef int (_cdecl * PUTENVPROC) (const char *);
	static struct
	{
		char	   *modulename;
		HMODULE		hmodule;
		PUTENVPROC	putenvFunc;
	}			rtmodules[] =
	{
		{
			"msvcrt", NULL, NULL
		},						/* Visual Studio 6.0 / MinGW */
		{
			"msvcrtd", NULL, NULL
		},
		{
			"msvcr70", NULL, NULL
		},						/* Visual Studio 2002 */
		{
			"msvcr70d", NULL, NULL
		},
		{
			"msvcr71", NULL, NULL
		},						/* Visual Studio 2003 */
		{
			"msvcr71d", NULL, NULL
		},
		{
			"msvcr80", NULL, NULL
		},						/* Visual Studio 2005 */
		{
			"msvcr80d", NULL, NULL
		},
		{
			"msvcr90", NULL, NULL
		},						/* Visual Studio 2008 */
		{
			"msvcr90d", NULL, NULL
		},
		{
			"msvcr100", NULL, NULL
		},						/* Visual Studio 2010 */
		{
			"msvcr100d", NULL, NULL
		},
		{
			"msvcr110", NULL, NULL
		},						/* Visual Studio 2012 */
		{
			"msvcr110d", NULL, NULL
		},
		{
			"msvcr120", NULL, NULL
		},						/* Visual Studio 2013 */
		{
			"msvcr120d", NULL, NULL
		},
		{
			"urctbase", 0, NULL
		},						/* Visual Studio 2015 and later */
		{
			NULL, 0, NULL
		}
	};
	int			i;

	for (i = 0; rtmodules[i].modulename; i++)
	{
		if (rtmodules[i].putenvFunc == NULL)
		{
			if (rtmodules[i].hmodule == NULL)
			{
				/* Not attempted before, so try to find this DLL */
				rtmodules[i].hmodule = GetModuleHandle(rtmodules[i].modulename);
				if (rtmodules[i].hmodule == NULL)
				{
					/*
					 * Set to INVALID_HANDLE_VALUE so we know we have tried
					 * this one before, and won't try again.
					 */
					rtmodules[i].hmodule = INVALID_HANDLE_VALUE;
					continue;
				}
				else
				{
					rtmodules[i].putenvFunc = (PUTENVPROC) GetProcAddress(rtmodules[i].hmodule, "_putenv");
					if (rtmodules[i].putenvFunc == NULL)
					{
						rtmodules[i].hmodule = INVALID_HANDLE_VALUE;
						continue;
					}
				}
			}
			else
			{
				/*
				 * Module loaded, but we did not find the function last time.
				 * We're not going to find it this time either...
				 */
				continue;
			}
		}
		/* At this point, putenvFunc is set or we have exited the loop */
		rtmodules[i].putenvFunc(envval);
	}
#endif   /* _MSC_VER */

	/*
	 * Update process environment, making this change visible to child
	 * processes and to CRTs initializing in the future.
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
	if (strlen(cp))
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

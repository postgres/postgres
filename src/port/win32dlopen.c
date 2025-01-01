/*-------------------------------------------------------------------------
 *
 * win32dlopen.c
 *	  dynamic loader for Windows
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/win32dlopen.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

static char last_dyn_error[512];

static void
set_dl_error(void)
{
	DWORD		err = GetLastError();

	if (FormatMessage(FORMAT_MESSAGE_IGNORE_INSERTS |
					  FORMAT_MESSAGE_FROM_SYSTEM,
					  NULL,
					  err,
					  MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT),
					  last_dyn_error,
					  sizeof(last_dyn_error) - 1,
					  NULL) == 0)
	{
		snprintf(last_dyn_error, sizeof(last_dyn_error) - 1,
				 "unknown error %lu", err);
	}
}

char *
dlerror(void)
{
	if (last_dyn_error[0])
		return last_dyn_error;
	else
		return NULL;
}

int
dlclose(void *handle)
{
	if (!FreeLibrary((HMODULE) handle))
	{
		set_dl_error();
		return 1;
	}
	last_dyn_error[0] = 0;
	return 0;
}

void *
dlsym(void *handle, const char *symbol)
{
	void	   *ptr;

	ptr = GetProcAddress((HMODULE) handle, symbol);
	if (!ptr)
	{
		set_dl_error();
		return NULL;
	}
	last_dyn_error[0] = 0;
	return ptr;
}

void *
dlopen(const char *file, int mode)
{
	HMODULE		h;
	int			prevmode;

	/* Disable popup error messages when loading DLLs */
	prevmode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
	h = LoadLibrary(file);
	SetErrorMode(prevmode);

	if (!h)
	{
		set_dl_error();
		return NULL;
	}
	last_dyn_error[0] = 0;
	return (void *) h;
}

/*-------------------------------------------------------------------------
 *
 * mingwcompat.c
 *	  MinGW compatibility functions
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/port/win32/mingwcompat.c,v 1.4 2008/01/01 19:45:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

/*
 * This file contains loaders for functions that are missing in the MinGW
 * import libraries. It's only for actual Win32 API functions, so they are
 * all present in proper Win32 compilers.
 */
#ifndef WIN32_ONLY_COMPILER

static HMODULE kernel32 = NULL;

/*
 * Load DLL file just once regardless of how many functions
 * we load/call in it.
 */
static void
LoadKernel32()
{
	if (kernel32 != NULL)
		return;

	kernel32 = LoadLibraryEx("kernel32.dll", NULL, 0);
	if (kernel32 == NULL)
		ereport(FATAL,
				(errmsg_internal("could not load kernel32.dll: %d",
								 (int) GetLastError())));
}


/*
 * Replacement for RegisterWaitForSingleObject(), which lives in
 * kernel32.dll·
 */
typedef
BOOL(WINAPI * __RegisterWaitForSingleObject)
(PHANDLE, HANDLE, WAITORTIMERCALLBACK, PVOID, ULONG, ULONG);
static __RegisterWaitForSingleObject _RegisterWaitForSingleObject = NULL;

BOOL		WINAPI
RegisterWaitForSingleObject(PHANDLE phNewWaitObject,
							HANDLE hObject,
							WAITORTIMERCALLBACK Callback,
							PVOID Context,
							ULONG dwMilliseconds,
							ULONG dwFlags)
{
	if (_RegisterWaitForSingleObject == NULL)
	{
		LoadKernel32();

		_RegisterWaitForSingleObject = (__RegisterWaitForSingleObject)
			GetProcAddress(kernel32, "RegisterWaitForSingleObject");

		if (_RegisterWaitForSingleObject == NULL)
			ereport(FATAL,
					(errmsg_internal("could not locate RegisterWaitForSingleObject in kernel32.dll: %d",
									 (int) GetLastError())));
	}

	return (_RegisterWaitForSingleObject)
		(phNewWaitObject, hObject, Callback, Context, dwMilliseconds, dwFlags);
}

#endif

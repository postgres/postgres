/*
 * gettimeofday.c
 *	  Win32 gettimeofday() replacement
 *
 * src/port/gettimeofday.c
 *
 * Copyright (c) 2003 SRA, Inc.
 * Copyright (c) 2003 SKC, Inc.
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose, without fee, and without a
 * written agreement is hereby granted, provided that the above
 * copyright notice and this paragraph and the following two
 * paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE TO ANY PARTY FOR DIRECT,
 * INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA HAS BEEN ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS
 * IS" BASIS, AND THE AUTHOR HAS NO OBLIGATIONS TO PROVIDE MAINTENANCE,
 * SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 */

#include "c.h"

#include <sys/time.h>


/* FILETIME of Jan 1 1970 00:00:00, the PostgreSQL epoch */
static const unsigned __int64 epoch = UINT64CONST(116444736000000000);

/*
 * FILETIME represents the number of 100-nanosecond intervals since
 * January 1, 1601 (UTC).
 */
#define FILETIME_UNITS_PER_SEC	10000000L
#define FILETIME_UNITS_PER_USEC	10

/*
 * Both GetSystemTimeAsFileTime and GetSystemTimePreciseAsFileTime share a
 * signature, so we can just store a pointer to whichever we find. This
 * is the pointer's type.
 */
typedef VOID (WINAPI *PgGetSystemTimeFn)(LPFILETIME);

/* Storage for the function we pick at runtime */
static PgGetSystemTimeFn pg_get_system_time = NULL;

/*
 * During backend startup, determine if GetSystemTimePreciseAsFileTime is
 * available and use it; if not, fall back to GetSystemTimeAsFileTime.
 */
void
init_win32_gettimeofday(void)
{
	/*
	 * Because it's guaranteed that kernel32.dll will be linked into our
	 * address space already, we don't need to LoadLibrary it and worry about
	 * closing it afterwards, so we're not using Pg's dlopen/dlsym() wrapper.
	 *
	 * We'll just look up the address of GetSystemTimePreciseAsFileTime if
	 * present.
	 *
	 * While we could look up the Windows version and skip this on Windows
	 * versions below Windows 8 / Windows Server 2012 there isn't much point,
	 * and determining the windows version is its self somewhat Windows version
	 * and development SDK specific...
	 */
	pg_get_system_time = (PgGetSystemTimeFn) GetProcAddress(
			GetModuleHandle(TEXT("kernel32.dll")),
				"GetSystemTimePreciseAsFileTime");
	if (pg_get_system_time == NULL)
	{
		/*
		 * The expected error from GetLastError() is ERROR_PROC_NOT_FOUND, if
		 * the function isn't present. No other error should occur.
		 *
		 * It's too early in startup to elog(...) if we get some unexpected
		 * error, and not serious enough to warrant a fprintf to stderr about
		 * it or save the error and report it later. So silently fall back to
		 * GetSystemTimeAsFileTime irrespective of why the failure occurred.
		 */
		pg_get_system_time = &GetSystemTimeAsFileTime;
	}

}

/*
 * timezone information is stored outside the kernel so tzp isn't used anymore.
 *
 * Note: this function is not for Win32 high precision timing purposes. See
 * elapsed_time().
 */
int
gettimeofday(struct timeval * tp, struct timezone * tzp)
{
	FILETIME	file_time;
	ULARGE_INTEGER ularge;

	(*pg_get_system_time)(&file_time);
	ularge.LowPart = file_time.dwLowDateTime;
	ularge.HighPart = file_time.dwHighDateTime;

	tp->tv_sec = (long) ((ularge.QuadPart - epoch) / FILETIME_UNITS_PER_SEC);
	tp->tv_usec = (long) (((ularge.QuadPart - epoch) % FILETIME_UNITS_PER_SEC)
		/ FILETIME_UNITS_PER_USEC);

	return 0;
}

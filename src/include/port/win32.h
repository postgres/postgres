/* src/include/port/win32.h */

/*
 * We always rely on the WIN32 macro being set by our build system,
 * but _WIN32 is the compiler pre-defined macro. So make sure we define
 * WIN32 whenever _WIN32 is set, to facilitate standalone building.
 */
#if defined(_WIN32) && !defined(WIN32)
#define WIN32
#endif

/*
 * Make sure _WIN32_WINNT has the minimum required value.
 * Leave a higher value in place. When building with at least Visual
 * Studio 2015 the minimum requirement is Windows Vista (0x0600) to
 * get support for GetLocaleInfoEx() with locales. For everything else
 * the minimum version is Windows XP (0x0501).
 */
#if defined(_MSC_VER) && _MSC_VER >= 1900
#define MIN_WINNT 0x0600
#else
#define MIN_WINNT 0x0501
#endif

#if defined(_WIN32_WINNT) && _WIN32_WINNT < MIN_WINNT
#undef _WIN32_WINNT
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT MIN_WINNT
#endif

/*
 * We need to prevent <crtdefs.h> from defining a symbol conflicting with
 * our errcode() function.  Since it's likely to get included by standard
 * system headers, pre-emptively include it now.
 */
#if defined(_MSC_VER) || defined(HAVE_CRTDEFS_H)
#define errcode __msvc_errcode
#include <crtdefs.h>
#undef errcode
#endif

/*
 * defines for dynamic linking on Win32 platform
 */

#ifdef BUILDING_DLL
#define PGDLLIMPORT __declspec (dllexport)
#else
#define PGDLLIMPORT __declspec (dllimport)
#endif

#ifdef _MSC_VER
#define PGDLLEXPORT __declspec (dllexport)
#else
#define PGDLLEXPORT
#endif

/*
 * Windows headers don't define this structure, but you can define it yourself
 * to use the functionality.
 */
struct sockaddr_un
{
	unsigned short sun_family;
	char		sun_path[108];
};
#define HAVE_STRUCT_SOCKADDR_UN 1

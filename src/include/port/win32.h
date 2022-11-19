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
 * Leave a higher value in place.  The minimum requirement is Windows 10.
 */
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif

#define _WIN32_WINNT 0x0A00

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

/*
 * Variables declared in the core backend and referenced by loadable
 * modules need to be marked "dllimport" in the core build, but
 * "dllexport" when the declaration is read in a loadable module.
 * No special markings should be used when compiling frontend code.
 */
#ifndef FRONTEND
#ifdef BUILDING_DLL
#define PGDLLIMPORT __declspec (dllexport)
#else
#define PGDLLIMPORT __declspec (dllimport)
#endif
#endif

/*
 * Functions exported by a loadable module must be marked "dllexport".
 *
 * While mingw would otherwise fall back to
 * __attribute__((visibility("default"))), that appears to only work as long
 * as no symbols are declared with __declspec(dllexport). But we can end up
 * with some, e.g. plpython's Py_Init.
 */
#define PGDLLEXPORT __declspec (dllexport)

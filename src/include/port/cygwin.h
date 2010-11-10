/* $PostgreSQL: pgsql/src/include/port/cygwin.h,v 1.9 2010/05/28 16:34:15 itagaki Exp $ */

#include <cygwin/version.h>

/*
 * Check for b20.1 and disable AF_UNIX family socket support.
 */
#if CYGWIN_VERSION_DLL_MAJOR < 1001
#undef HAVE_UNIX_SOCKETS
#endif

#if __GNUC__ && ! defined (__declspec)
#error You need egcs 1.1 or newer for compiling!
#endif

#ifdef BUILDING_DLL
#define PGDLLIMPORT __declspec (dllexport)
#else
#define PGDLLIMPORT __declspec (dllimport)
#endif

#define PGDLLEXPORT

/* $Header: /cvsroot/pgsql/src/include/port/cygwin.h,v 1.1 2003/03/21 17:18:34 petere Exp $ */

#define HAS_TEST_AND_SET
typedef unsigned char slock_t;

#define tzname _tzname			/* should be in time.h? */
#define HAVE_INT_TIMEZONE		/* has int _timezone */

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
#define DLLIMPORT __declspec (dllexport)
#else
#define DLLIMPORT __declspec (dllimport)
#endif

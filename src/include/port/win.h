#define HAS_TEST_AND_SET
typedef unsigned char slock_t;

#ifndef O_DIROPEN
#define O_DIROPEN	0x100000	/* should be in sys/fcntl.h */
#endif

#define tzname _tzname			/* should be in time.h? */
#define HAVE_INT_TIMEZONE		/* has int _timezone */

#include <cygwin/version.h>

/*
 * Check for b20.1 and disable AF_UNIX family socket support.
 */
#if CYGWIN_VERSION_DLL_MAJOR < 1001
#undef HAVE_UNIX_SOCKETS
#endif

/* defines for dynamic linking on Win32 platform */
#ifdef __CYGWIN__

#if __GNUC__ && ! defined (__declspec)
#error You need egcs 1.1 or newer for compiling!
#endif

#ifdef BUILDING_DLL
#define DLLIMPORT __declspec (dllexport)
#else							/* not BUILDING_DLL */
#define DLLIMPORT __declspec (dllimport)
#endif

#elif defined(WIN32) && defined(_MSC_VER)		/* not CYGWIN */

#if defined(_DLL)
#define DLLIMPORT __declspec (dllexport)
#else							/* not _DLL */
#define DLLIMPORT __declspec (dllimport)
#endif

#else							/* not CYGWIN, not MSVC */

#define DLLIMPORT

#endif

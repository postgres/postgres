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

#define JMP_BUF
#define HAS_TEST_AND_SET
typedef unsigned char slock_t;

#ifndef O_DIROPEN
#define O_DIROPEN	0x100000	/* should be in sys/fcntl.h */
#endif

#define tzname _tzname			/* should be in time.h? */
#define USE_POSIX_TIME
#define HAVE_INT_TIMEZONE		/* has int _timezone */

#include <cygwin/version.h>
#if (CYGWIN_VERSION_API_MAJOR >= 0) && (CYGWIN_VERSION_API_MINOR >= 8)
#define sys_nerr _sys_nerr
#endif

/* not exported in readline.h */
char * filename_completion_function();

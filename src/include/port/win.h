#define JMP_BUF
#define HAS_TEST_AND_SET
typedef unsigned char slock_t;

#ifndef O_DIROPEN
#define O_DIROPEN	0x100000	/* should be in sys/fcntl.h */
#endif

#define tzname _tzname			/* should be in time.h? */
#define USE_POSIX_TIME
#define HAVE_INT_TIMEZONE		/* has int _timezone */

#define USE_POSIX_TIME
#define DISABLE_XOPEN_NLS
#define HAS_TEST_AND_SET
#include <sys/mman.h>			/* for msemaphore */
typedef msemaphore slock_t;

/* some platforms define __alpha, but not __alpha__ */
#if defined(__alpha) && !defined(__alpha__)
#define __alpha__
#endif


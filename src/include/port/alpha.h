#define USE_POSIX_TIME
#define USE_POSIX_SIGNALS
#define DISABLE_XOPEN_NLS
#define HAS_LONG_LONG
#define HAS_TEST_AND_SET
#include <sys/mman.h>			/* for msemaphore */
typedef msemaphore slock_t;

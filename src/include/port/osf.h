#define NOFIXADE
#define DISABLE_XOPEN_NLS
#define HAS_TEST_AND_SET
/*#include <sys/mman.h>*/			/* for msemaphore */
/*typedef msemaphore slock_t;*/
#include <alpha/builtins.h>
typedef volatile long slock_t;

#  define USE_POSIX_TIME 
#  define USE_POSIX_SIGNALS
#  define NO_EMPTY_STMTS
#  define SYSV_DIRENT
#  define HAS_TEST_AND_SET
#  include <abi_mutex.h>
   typedef abilock_t slock_t;

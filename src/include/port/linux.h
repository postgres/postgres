/* __USE_POSIX, __USE_BSD, and __USE_BSD_SIGNAL used to be defined either
   here or with -D compile options, but __ macros should be set and used by C
   library macros, not Postgres code.  __USE_POSIX is set by features.h,
   __USE_BSD is set by bsd/signal.h, and __USE_BSD_SIGNAL appears not to
   be used.
*/
#  define JMP_BUF
#  define USE_POSIX_TIME
#  define USE_POSIX_SIGNALS
#  if !defined(PPC)
#    define NEED_I386_TAS_ASM
#    define HAS_TEST_AND_SET
     typedef unsigned char slock_t;
#  endif

#  define USE_POSIX_TIME
#  define NEED_I386_TAS_ASM
#  define HAS_TEST_AND_SET
#  if defined(__mips__)
/* #    undef HAS_TEST_AND_SET */
#  endif
   typedef unsigned char slock_t;

/* __USE_POSIX, __USE_BSD, and __USE_BSD_SIGNAL used to be defined either
   here or with -D compile options, but __ macros should be set and used by C
   library macros, not Postgres code.  __USE_POSIX is set by features.h,
   __USE_BSD is set by bsd/signal.h, and __USE_BSD_SIGNAL appears not to
   be used.
*/
#define JMP_BUF
#define USE_POSIX_TIME

#if defined(__i386__)
typedef unsigned char slock_t;
#define HAS_TEST_AND_SET

#elif defined(__sparc__)
typedef unsigned char slock_t;
#define HAS_TEST_AND_SET

#elif defined(__powerpc__)
typedef unsigned int slock_t;
#define HAS_TEST_AND_SET

#elif defined(__alpha__)
typedef long int slock_t;
#define HAS_TEST_AND_SET

#elif defined(__mips__)
typedef unsigned int slock_t;
#define HAS_TEST_AND_SET

#endif

#if (__GLIBC__ >= 2)
#ifdef HAVE_INT_TIMEZONE
#undef HAVE_INT_TIMEZONE
#endif

 /*
  * currently undefined as I (teunis@computersupportcentre.com) have not
  * checked this yet
  */
/* #define HAVE_SIGSETJMP 1 */
#endif

#if defined(__powerpc__)
#undef HAVE_INT_TIMEZONE
#endif
